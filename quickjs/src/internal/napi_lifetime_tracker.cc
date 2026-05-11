#include "internal/napi_lifetime_tracker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

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
lifetime_counter &counter_for(napi_lifetime_kind kind);

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
struct allocator_slot_counter
{
  std::atomic<size_t> total_slots{0};
  std::atomic<size_t> active_slots{0};
};

allocator_slot_counter value_slots;
allocator_slot_counter ref_slots;
allocator_slot_counter scope_slots;
std::atomic<int64_t> last_periodic_stats_ms{0};

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
constexpr int k_min_known_tag = -9;
constexpr int k_max_known_tag = 8;
constexpr size_t k_known_tag_count =
    static_cast<size_t>(k_max_known_tag - k_min_known_tag + 1);
constexpr size_t k_unknown_tag_index = k_known_tag_count;
constexpr size_t k_tag_bucket_count = k_known_tag_count + 1;

struct per_scope_tag_counters
{
  size_t scope_index = 0;
  size_t value_tag_slots[k_tag_bucket_count] = {};
  size_t ref_tag_slots[k_tag_bucket_count] = {};
};

std::mutex tag_stats_mutex;
std::vector<per_scope_tag_counters> tag_stats_by_scope;

size_t tag_bucket_index(int tag)
{
  if (tag < k_min_known_tag || tag > k_max_known_tag)
    return k_unknown_tag_index;
  return static_cast<size_t>(tag - k_min_known_tag);
}

const char *tag_bucket_name(size_t index)
{
  if (index == k_unknown_tag_index)
    return "unknown";

  switch (static_cast<int>(index) + k_min_known_tag)
  {
  case -9:
    return "big_int";
  case -8:
    return "symbol";
  case -7:
    return "string";
  case -6:
    return "string_rope";
  case -5:
    return "tag_-5";
  case -4:
    return "tag_-4";
  case -3:
    return "module";
  case -2:
    return "function_bytecode";
  case -1:
    return "object";
  case 0:
    return "int";
  case 1:
    return "bool";
  case 2:
    return "null";
  case 3:
    return "undefined";
  case 4:
    return "uninitialized";
  case 5:
    return "catch_offset";
  case 6:
    return "exception";
  case 7:
    return "short_big_int";
  case 8:
    return "float64";
  default:
    return "unknown";
  }
}

per_scope_tag_counters &tag_counters_for_scope(size_t scope_index)
{
  for (auto &scope : tag_stats_by_scope)
  {
    if (scope.scope_index == scope_index)
      return scope;
  }

  tag_stats_by_scope.push_back({});
  tag_stats_by_scope.back().scope_index = scope_index;
  return tag_stats_by_scope.back();
}

size_t *tag_counter_for(per_scope_tag_counters &scope, napi_lifetime_tag_owner_kind kind)
{
  return kind == napi_lifetime_tag_owner_kind::ref ? scope.ref_tag_slots : scope.value_tag_slots;
}

void adjust_tag_counter(size_t &counter, std::ptrdiff_t delta)
{
  if (delta > 0)
  {
    counter += static_cast<size_t>(delta);
    return;
  }

  if (delta == 0)
    return;

  size_t decrement = static_cast<size_t>(-delta);
  counter = decrement >= counter ? 0 : counter - decrement;
}

bool tag_counters_have_values(const size_t *counters)
{
  for (size_t i = 0; i < k_tag_bucket_count; ++i)
  {
    if (counters[i] != 0)
      return true;
  }
  return false;
}

void dump_scope_tag_line(size_t scope_index, const char *label, const size_t *counters)
{
  std::fprintf(stderr, "[napi-lifetime-tags] scope=%zu %s", scope_index, label);
  bool printed = false;
  for (size_t i = 0; i < k_tag_bucket_count; ++i)
  {
    size_t count = counters[i];
    if (count == 0)
      continue;
    std::fprintf(stderr, " %s=%zu", tag_bucket_name(i), count);
    printed = true;
  }
  if (!printed)
    std::fprintf(stderr, " none=0");
  std::fprintf(stderr, "\n");
}

void dump_scope_tag_lines()
{
  std::lock_guard<std::mutex> lock(tag_stats_mutex);
  for (const auto &scope : tag_stats_by_scope)
  {
    if (tag_counters_have_values(scope.value_tag_slots))
      dump_scope_tag_line(scope.scope_index, "napi_value", scope.value_tag_slots);
    if (tag_counters_have_values(scope.ref_tag_slots))
      dump_scope_tag_line(scope.scope_index, "napi_ref", scope.ref_tag_slots);
  }
}
#endif

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
  switch (kind)
  {
  case napi_lifetime_slot_kind::ref:
    return ref_slots;
  case napi_lifetime_slot_kind::scope:
    return scope_slots;
  case napi_lifetime_slot_kind::value:
  default:
    return value_slots;
  }
}

void maybe_dump_periodic_stats()
{
  constexpr int64_t interval_ms = 10000;
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
               "napi_ref slots_total=%zu active=%zu "
               "napi_scope slots_total=%zu active=%zu\n",
               value_slots.total_slots.load(std::memory_order_relaxed),
               value_slots.active_slots.load(std::memory_order_relaxed),
               ref_slots.total_slots.load(std::memory_order_relaxed),
               ref_slots.active_slots.load(std::memory_order_relaxed),
               scope_slots.total_slots.load(std::memory_order_relaxed),
               scope_slots.active_slots.load(std::memory_order_relaxed));

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
  dump_scope_tag_lines();
#endif
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

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
  if (kind == napi_lifetime_kind::scope && periodic_stats_enabled())
    maybe_dump_periodic_stats();
#endif

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

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
  if (kind == napi_lifetime_kind::scope && periodic_stats_enabled())
    maybe_dump_periodic_stats();
#endif

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

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
void napi_lifetime_tracker__::record_value_tag_delta(napi_lifetime_tag_owner_kind kind,
                                                     size_t scope_index,
                                                     int tag,
                                                     std::ptrdiff_t active_delta)
{
  if (!periodic_stats_enabled())
    return;

  {
    std::lock_guard<std::mutex> lock(tag_stats_mutex);
    per_scope_tag_counters &scope = tag_counters_for_scope(scope_index);
    size_t *counters = tag_counter_for(scope, kind);
    adjust_tag_counter(counters[tag_bucket_index(tag)], active_delta);
  }
  maybe_dump_periodic_stats();
}
#endif
#endif

} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(const char *reason)
{
  quickjs::detail::napi_lifetime_tracker__::dump(reason);
}
