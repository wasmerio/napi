#include "internal/napi_lifetime_tracker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace quickjs::detail
{
namespace
{
struct lifetime_counter
{
  const char *name;
  std::atomic<size_t> created{0};
  std::atomic<size_t> destroyed{0};
  std::atomic<size_t> live{0};
  std::atomic<size_t> peak{0};
};

lifetime_counter counters[] = {
    {"napi_env__"},
    {"napi_scope__"},
    {"napi_handle_scope__"},
    {"napi_escapable_handle_scope__"},
    {"napi_value__"},
    {"napi_ref__"},
    {"napi_callback_info__"},
    {"napi_external_backing_store_hint__"},
    {"napi_deferred__"},
    {"napi_env_cleanup_hook__"},
};

constexpr size_t k_counter_count = sizeof(counters) / sizeof(counters[0]);

bool enabled();

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
struct allocator_slot_counter
{
  std::atomic<size_t> total_slots{0};
  std::atomic<size_t> active_slots{0};
};

allocator_slot_counter value_slots;
allocator_slot_counter ref_slots;
std::atomic<int64_t> last_periodic_stats_ms{0};

int64_t monotonic_milliseconds()
{
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

void adjust_counter(std::atomic<size_t> &counter, std::ptrdiff_t delta)
{
  if (delta > 0)
  {
    counter.fetch_add(static_cast<size_t>(delta), std::memory_order_relaxed);
    return;
  }

  if (delta == 0)
    return;

  size_t decrement = static_cast<size_t>(-delta);
  size_t current = counter.load(std::memory_order_relaxed);
  while (current > 0)
  {
    size_t next = decrement >= current ? 0 : current - decrement;
    if (counter.compare_exchange_weak(
            current, next, std::memory_order_relaxed, std::memory_order_relaxed))
      return;
  }
}

allocator_slot_counter &slot_counter_for(napi_lifetime_slot_kind kind)
{
  return kind == napi_lifetime_slot_kind::ref ? ref_slots : value_slots;
}

void maybe_dump_periodic_stats()
{
  constexpr int64_t interval_ms = 2000;
  int64_t now = monotonic_milliseconds();
  int64_t last = last_periodic_stats_ms.load(std::memory_order_relaxed);
  if (last == 0)
  {
    last_periodic_stats_ms.compare_exchange_strong(
        last, now, std::memory_order_relaxed, std::memory_order_relaxed);
    return;
  }

  if (now - last < interval_ms)
    return;

  if (!last_periodic_stats_ms.compare_exchange_strong(
          last, now, std::memory_order_relaxed, std::memory_order_relaxed))
    return;

  std::fprintf(stderr,
               "[napi-lifetime-stats] napi_value slots_total=%zu active=%zu "
               "napi_ref slots_total=%zu active=%zu\n",
               value_slots.total_slots.load(std::memory_order_relaxed),
               value_slots.active_slots.load(std::memory_order_relaxed),
               ref_slots.total_slots.load(std::memory_order_relaxed),
               ref_slots.active_slots.load(std::memory_order_relaxed));
}

bool periodic_stats_enabled()
{
  const char *value = std::getenv("EDGE_TRACE_NAPI_LIFETIME_STATS");
  if (value != nullptr && value[0] != '\0')
    return value[0] != '0';
  return enabled();
}
#endif

bool enabled()
{
  const char *value = std::getenv("EDGE_TRACE_NAPI_LIFETIME");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

lifetime_counter &counter_for(napi_lifetime_kind kind)
{
  size_t index = static_cast<size_t>(kind);
  if (index >= k_counter_count)
    index = 0;
  return counters[index];
}

void update_peak(lifetime_counter &counter, size_t live)
{
  size_t peak = counter.peak.load(std::memory_order_relaxed);
  while (live > peak &&
         !counter.peak.compare_exchange_weak(
             peak, live, std::memory_order_relaxed, std::memory_order_relaxed))
  {
  }
}

size_t decrement_live(lifetime_counter &counter)
{
  size_t live = counter.live.load(std::memory_order_relaxed);
  while (live > 0)
  {
    if (counter.live.compare_exchange_weak(
            live, live - 1, std::memory_order_relaxed, std::memory_order_relaxed))
      return live - 1;
  }
  return 0;
}

size_t dump_every()
{
  const char *value = std::getenv("EDGE_TRACE_NAPI_LIFETIME_DUMP_EVERY");
  if (value == nullptr || value[0] == '\0')
    return 0;

  char *end = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value)
    return 0;
  return static_cast<size_t>(parsed);
}
} // namespace

void napi_lifetime_tracker__::record_create(napi_lifetime_kind kind, const void *ptr, const void *env)
{
  auto &counter = counter_for(kind);
  size_t created = counter.created.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t live = counter.live.fetch_add(1, std::memory_order_relaxed) + 1;
  update_peak(counter, live);

  if (!enabled())
    return;

  std::fprintf(stderr,
               "[napi-lifetime] + %-36s ptr=%p env=%p live=%zu created=%zu\n",
               counter.name,
               ptr,
               env,
               live,
               created);

  size_t interval = dump_every();
  if (interval != 0 && created % interval == 0)
    dump("periodic create interval");
}

void napi_lifetime_tracker__::record_destroy(napi_lifetime_kind kind, const void *ptr, const void *env)
{
  auto &counter = counter_for(kind);
  size_t destroyed = counter.destroyed.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t live = decrement_live(counter);

  if (!enabled())
    return;

  std::fprintf(stderr,
               "[napi-lifetime] - %-36s ptr=%p env=%p live=%zu destroyed=%zu\n",
               counter.name,
               ptr,
               env,
               live,
               destroyed);
}

void napi_lifetime_tracker__::dump(const char *reason)
{
  if (!enabled())
    return;

  std::fprintf(stderr,
               "[napi-lifetime] dump reason=%s\n",
               reason == nullptr ? "(none)" : reason);
  for (const auto &counter : counters)
  {
    std::fprintf(stderr,
                 "[napi-lifetime]   %-36s live=%zu peak=%zu created=%zu destroyed=%zu\n",
                 counter.name,
                 counter.live.load(std::memory_order_relaxed),
                 counter.peak.load(std::memory_order_relaxed),
                 counter.created.load(std::memory_order_relaxed),
                 counter.destroyed.load(std::memory_order_relaxed));
  }
}

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
void napi_lifetime_tracker__::record_allocator_slot_delta(napi_lifetime_slot_kind kind,
                                                          std::ptrdiff_t total_delta,
                                                          std::ptrdiff_t active_delta)
{
  if (!periodic_stats_enabled())
    return;

  allocator_slot_counter &counter = slot_counter_for(kind);
  adjust_counter(counter.total_slots, total_delta);
  adjust_counter(counter.active_slots, active_delta);
  maybe_dump_periodic_stats();
}
#endif

} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(const char *reason)
{
  quickjs::detail::napi_lifetime_tracker__::dump(reason);
}
