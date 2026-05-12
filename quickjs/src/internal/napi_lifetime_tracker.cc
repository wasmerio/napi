#include "internal/napi_lifetime_tracker.h"

#include "internal/napi_deferred.h"
#include "internal/napi_env.h"
#include "internal/napi_env_cleanup_hook.h"
#include "internal/napi_external_backing_store_hint.h"
#include "internal/napi_ref.h"
#include "internal/napi_scope.h"
#include "internal/napi_value.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quickjs::detail
{
namespace
{
bool enabled()
{
  const char *value = std::getenv("EDGE_TRACE_NAPI_LIFETIME");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool periodic_stats_enabled()
{
  const char *value = std::getenv("EDGE_TRACE_NAPI_LIFETIME_STATS");
  if (value != nullptr && value[0] != '\0')
    return value[0] != '0';
  return enabled();
}

int64_t monotonic_milliseconds()
{
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS) || \
    defined(NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP)
constexpr int k_min_known_tag = -9;
constexpr int k_max_known_tag = 8;
constexpr size_t k_known_tag_count =
    static_cast<size_t>(k_max_known_tag - k_min_known_tag + 1);
constexpr size_t k_unknown_tag_index = k_known_tag_count;
constexpr size_t k_tag_bucket_count = k_known_tag_count + 1;

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

struct tag_counters
{
  size_t slots[k_tag_bucket_count] = {};
};

void add_tag(tag_counters &counters, int tag)
{
  ++counters.slots[tag_bucket_index(tag)];
}

void remove_tag(tag_counters &counters, int tag)
{
  size_t &slot = counters.slots[tag_bucket_index(tag)];
  if (slot > 0)
    --slot;
}

bool has_tags(const tag_counters &counters)
{
  for (size_t i = 0; i < k_tag_bucket_count; ++i)
  {
    if (counters.slots[i] != 0)
      return true;
  }
  return false;
}
#endif

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
constexpr size_t k_value_dump_max_bytes = 240;

struct string_symbol_entry
{
  int tag = 0;
  std::string value;
  size_t count = 0;
};

struct object_type_entry
{
  std::string prototype_name;
  size_t count = 0;
};

void append_hex_escape(std::string &out, unsigned char c)
{
  constexpr char hex[] = "0123456789abcdef";
  out.push_back('\\');
  out.push_back('x');
  out.push_back(hex[(c >> 4) & 0x0f]);
  out.push_back(hex[c & 0x0f]);
}

std::string escaped_value_fragment(const char *value, size_t value_length)
{
  size_t limit = value_length < k_value_dump_max_bytes ? value_length : k_value_dump_max_bytes;
  std::string out;
  out.reserve(limit + 3);
  for (size_t i = 0; i < limit; ++i)
  {
    unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c)
    {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c >= 0x20 && c <= 0x7e)
        out.push_back(static_cast<char>(c));
      else
        append_hex_escape(out, c);
      break;
    }
  }
  if (value_length > limit)
    out += "...";
  return out;
}

void add_string_symbol_entry(std::vector<string_symbol_entry> &entries,
                             int tag,
                             const std::string &value)
{
  for (auto &entry : entries)
  {
    if (entry.tag == tag && entry.value == value)
    {
      ++entry.count;
      return;
    }
  }

  string_symbol_entry entry;
  entry.tag = tag;
  entry.value = value;
  entry.count = 1;
  entries.push_back(std::move(entry));
}

void remove_string_symbol_entry(std::vector<string_symbol_entry> &entries,
                                int tag,
                                const std::string &value)
{
  for (auto it = entries.begin(); it != entries.end(); ++it)
  {
    if (it->tag == tag && it->value == value)
    {
      if (it->count > 1)
        --it->count;
      else
        entries.erase(it);
      return;
    }
  }
}

void add_object_type_entry(std::vector<object_type_entry> &entries,
                           const std::string &prototype_name)
{
  for (auto &entry : entries)
  {
    if (entry.prototype_name == prototype_name)
    {
      ++entry.count;
      return;
    }
  }

  object_type_entry entry;
  entry.prototype_name = prototype_name;
  entry.count = 1;
  entries.push_back(std::move(entry));
}

void remove_object_type_entry(std::vector<object_type_entry> &entries,
                              const std::string &prototype_name)
{
  for (auto it = entries.begin(); it != entries.end(); ++it)
  {
    if (it->prototype_name == prototype_name)
    {
      if (it->count > 1)
        --it->count;
      else
        entries.erase(it);
      return;
    }
  }
}

void clear_exception(JSContext *ctx)
{
  if (ctx == nullptr || !JS_HasException(ctx))
    return;
  JSValue exception = JS_GetException(ctx);
  JS_FreeValue(ctx, exception);
}

bool js_value_to_escaped_string(JSContext *ctx, JSValueConst value, std::string &out)
{
  size_t text_length = 0;
  const char *text = JS_ToCStringLen(ctx, &text_length, value);
  if (text == nullptr)
  {
    clear_exception(ctx);
    return false;
  }

  out = escaped_value_fragment(text, text_length);
  JS_FreeCString(ctx, text);
  return true;
}

std::string class_name_fallback(JSContext *ctx, JSValueConst value)
{
  JSRuntime *rt = JS_GetRuntime(ctx);
  JSClassID class_id = JS_GetClassID(value);
  JSAtom class_name = JS_GetClassName(rt, class_id);
  if (class_name == JS_ATOM_NULL)
    return "<object>";

  const char *text = JS_AtomToCString(ctx, class_name);
  std::string result = text == nullptr ? "<object>" : escaped_value_fragment(text, std::strlen(text));
  if (text != nullptr)
    JS_FreeCString(ctx, text);
  JS_FreeAtomRT(rt, class_name);
  clear_exception(ctx);
  return result.empty() ? "<object>" : result;
}

std::string object_prototype_name(napi_env env, JSValueConst value)
{
  JSContext *ctx = env->context();
  JSValue proto = JS_GetPrototype(ctx, value);
  if (JS_IsException(proto))
  {
    clear_exception(ctx);
    return class_name_fallback(ctx, value);
  }

  if (JS_IsNull(proto) || JS_IsUndefined(proto))
  {
    JS_FreeValue(ctx, proto);
    return "<null-prototype>";
  }

  JSValue ctor = JS_GetPropertyStr(ctx, proto, "constructor");
  JS_FreeValue(ctx, proto);
  if (JS_IsException(ctor))
  {
    clear_exception(ctx);
    return class_name_fallback(ctx, value);
  }

  JSValue name = JS_UNDEFINED;
  if (JS_IsObject(ctor))
    name = JS_GetPropertyStr(ctx, ctor, "name");
  JS_FreeValue(ctx, ctor);
  if (JS_IsException(name))
  {
    clear_exception(ctx);
    return class_name_fallback(ctx, value);
  }

  std::string result;
  bool has_name = !JS_IsUndefined(name) &&
                  !JS_IsNull(name) &&
                  js_value_to_escaped_string(ctx, name, result) &&
                  !result.empty();
  JS_FreeValue(ctx, name);

  return has_name ? result : class_name_fallback(ctx, value);
}
#endif

struct value_snapshot
{
  napi_env env = nullptr;
  int tag = JS_TAG_UNDEFINED;
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  bool has_string_symbol = false;
  std::string string_symbol_value;
  bool has_object_type = false;
  std::string object_type;
#endif
};

struct basic_snapshot
{
  napi_env env = nullptr;
};

template <typename Snapshot>
struct tracked_type_stats
{
  size_t created = 0;
  size_t released = 0;
  size_t active = 0;
  size_t peak = 0;
  std::unordered_map<const void *, Snapshot> live;
};

struct value_type_stats : tracked_type_stats<value_snapshot>
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
  tag_counters tags;
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  std::vector<string_symbol_entry> string_symbols;
  std::vector<object_type_entry> object_types;
#endif
};

struct lifetime_stats
{
  std::mutex mutex;
  value_type_stats values;
  value_type_stats refs;
  tracked_type_stats<basic_snapshot> cleanup_hooks;
  tracked_type_stats<basic_snapshot> deferreds;
  tracked_type_stats<basic_snapshot> external_backing_store_hints;
  size_t scope_escape_calls = 0;
  size_t scope_escape_succeeded = 0;
  size_t scope_escape_failed = 0;
  std::unordered_map<std::string, std::array<size_t, 3>> counter_history;
  std::unordered_map<std::string, size_t> counter_history_size;
  std::unordered_map<std::string, size_t> counter_history_next;
};

lifetime_stats g_lifetime;

struct counter_trend
{
  bool available = false;
  size_t current = 0;
  long long speed = 0;
  long long acceleration = 0;
};

counter_trend observe_counter(const std::string &key, size_t value)
{
  counter_trend trend;
  size_t &history_size = g_lifetime.counter_history_size[key];
  size_t &next = g_lifetime.counter_history_next[key];
  auto &history = g_lifetime.counter_history[key];

  history[next] = value;
  next = (next + 1) % history.size();
  if (history_size < history.size())
    ++history_size;

  if (history_size == history.size())
  {
    size_t i_minus_2_index = next;
    size_t i_minus_1_index = (next + 1) % history.size();
    size_t i_index = (next + 2) % history.size();
    long long x_i_minus_2 = static_cast<long long>(history[i_minus_2_index]);
    long long x_i_minus_1 = static_cast<long long>(history[i_minus_1_index]);
    long long x_i = static_cast<long long>(history[i_index]);
    trend.available = true;
    trend.current = history[i_minus_1_index];
    trend.speed = x_i - x_i_minus_2;
    trend.acceleration = x_i - 2 * x_i_minus_1 + x_i_minus_2;
  }
  return trend;
}

void format_center_value(char *buffer, size_t buffer_size, const counter_trend &trend)
{
  if (!trend.available)
  {
    std::snprintf(buffer, buffer_size, "%s", "-");
    return;
  }

  std::snprintf(buffer, buffer_size, "%zu", trend.current);
}

void format_delta_value(char *buffer, size_t buffer_size, const counter_trend &trend, bool acceleration)
{
  if (!trend.available)
  {
    std::snprintf(buffer, buffer_size, "%s", "-");
    return;
  }

  std::snprintf(buffer,
                buffer_size,
                "%lld",
                acceleration ? trend.acceleration : trend.speed);
}

void dump_metric_row(const char *metric, size_t value, const std::string &key)
{
  counter_trend trend = observe_counter(key, value);
  char current[32];
  char speed[32];
  char acceleration[32];
  format_center_value(current, sizeof(current), trend);
  format_delta_value(speed, sizeof(speed), trend, false);
  format_delta_value(acceleration, sizeof(acceleration), trend, true);
  std::fprintf(stderr,
               "  %-36s %10s %10s %10s\n",
               metric,
               current,
               speed,
               acceleration);
}

void dump_counter_header(const char *title)
{
  std::fprintf(stderr, "%s\n", title);
  std::fprintf(stderr, "  %-36s %10s %10s %10s\n", "metric", "x[i-1]", "speed", "accel");
}

value_snapshot capture_value_snapshot(napi_env env, JSValueConst value)
{
  value_snapshot snapshot;
  snapshot.env = env;
  snapshot.tag = JS_VALUE_GET_NORM_TAG(value);

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (env != nullptr && env->context() != nullptr)
  {
    JSContext *ctx = env->context();
    if ((snapshot.tag == JS_TAG_STRING ||
         snapshot.tag == JS_TAG_STRING_ROPE) &&
        js_value_to_escaped_string(ctx, value, snapshot.string_symbol_value))
    {
      snapshot.has_string_symbol = true;
    }

    if (snapshot.tag == JS_TAG_OBJECT)
    {
      snapshot.object_type = object_prototype_name(env, value);
      snapshot.has_object_type = true;
    }
  }
#endif

  return snapshot;
}

void add_value_snapshot(value_type_stats &stats, const value_snapshot &snapshot)
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
  add_tag(stats.tags, snapshot.tag);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (snapshot.has_string_symbol)
    add_string_symbol_entry(stats.string_symbols, snapshot.tag, snapshot.string_symbol_value);
  if (snapshot.has_object_type)
    add_object_type_entry(stats.object_types, snapshot.object_type);
#endif
}

void remove_value_snapshot(value_type_stats &stats, const value_snapshot &snapshot)
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
  remove_tag(stats.tags, snapshot.tag);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (snapshot.has_string_symbol)
    remove_string_symbol_entry(stats.string_symbols, snapshot.tag, snapshot.string_symbol_value);
  if (snapshot.has_object_type)
    remove_object_type_entry(stats.object_types, snapshot.object_type);
#endif
}

template <typename Stats, typename Snapshot, typename AddSnapshot>
void record_create_locked(Stats &stats,
                          const void *handle,
                          Snapshot snapshot,
                          AddSnapshot add_snapshot)
{
  auto existing = stats.live.find(handle);
  if (existing != stats.live.end())
  {
    if (stats.active > 0)
      --stats.active;
    stats.live.erase(existing);
  }

  ++stats.created;
  ++stats.active;
  stats.peak = std::max(stats.peak, stats.active);
  add_snapshot(stats, snapshot);
  stats.live.emplace(handle, std::move(snapshot));
}

template <typename Stats, typename RemoveSnapshot>
napi_env record_release_locked(Stats &stats,
                               const void *handle,
                               RemoveSnapshot remove_snapshot)
{
  auto existing = stats.live.find(handle);
  if (existing == stats.live.end())
    return nullptr;

  napi_env env = existing->second.env;
  remove_snapshot(stats, existing->second);
  stats.live.erase(existing);
  ++stats.released;
  if (stats.active > 0)
    --stats.active;
  return env;
}

void add_basic_snapshot(tracked_type_stats<basic_snapshot> &, const basic_snapshot &)
{
}

void remove_basic_snapshot(tracked_type_stats<basic_snapshot> &, const basic_snapshot &)
{
}

struct env_slot_scan
{
  size_t value_slots_total = 0;
  size_t active_values = 0;
  size_t ref_slots_total = 0;
  size_t active_refs = 0;
  size_t scope_slots_total = 0;
  size_t active_scopes = 0;
  std::vector<std::pair<size_t, size_t>> active_values_by_scope_level;
};

env_slot_scan scan_env_slots(napi_env env)
{
  env_slot_scan scan;
  if (env == nullptr)
    return scan;

  scan.scope_slots_total = env->scope_storage_slot_count();
  scan.active_scopes = env->active_scope_count();
  scan.ref_slots_total = env->ref_storage_slot_count();
  scan.active_refs = env->active_ref_count();
  std::unordered_map<size_t, size_t> active_values_by_scope_level;
  env->for_each_active_scope([&](const napi_scope__ &scope) {
    size_t active_values = scope.active_value_count();
    scan.value_slots_total += scope.value_storage_slot_count();
    scan.active_values += active_values;
    active_values_by_scope_level[scope.level()] += active_values;
  });
  scan.active_values_by_scope_level.reserve(active_values_by_scope_level.size());
  for (const auto &[level, count] : active_values_by_scope_level)
    scan.active_values_by_scope_level.push_back({level, count});
  std::sort(scan.active_values_by_scope_level.begin(),
            scan.active_values_by_scope_level.end(),
            [](const auto &left, const auto &right) {
              return left.first < right.first;
            });
  return scan;
}

template <typename Stats>
void dump_type_row(const char *label, const Stats &stats)
{
  counter_trend trend = observe_counter(std::string("type.") + label + ".active", stats.active);
  char current[32];
  char speed[32];
  char acceleration[32];
  format_center_value(current, sizeof(current), trend);
  format_delta_value(speed, sizeof(speed), trend, false);
  format_delta_value(acceleration, sizeof(acceleration), trend, true);
  std::fprintf(stderr,
               "  %-33s %10zu %10zu %10s %10zu %10s %10s\n",
               label,
               stats.created,
               stats.released,
               current,
               stats.peak,
               speed,
               acceleration);
}

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
void dump_tag_table(const char *label, const tag_counters &counters)
{
  if (!has_tags(counters))
    return;

  std::fprintf(stderr, "[napi-lifetime-tags] owner=%s\n", label);
  std::fprintf(stderr, "  %-16s %10s %10s %10s\n", "tag", "x[i-1]", "speed", "accel");
  for (size_t i = 0; i < k_tag_bucket_count; ++i)
  {
    size_t count = counters.slots[i];
    if (count == 0)
      continue;
    const char *name = tag_bucket_name(i);
    counter_trend trend = observe_counter(std::string("tag.") + label + "." + name, count);
    char current[32];
    char speed[32];
    char acceleration[32];
    format_center_value(current, sizeof(current), trend);
    format_delta_value(speed, sizeof(speed), trend, false);
    format_delta_value(acceleration, sizeof(acceleration), trend, true);
    std::fprintf(stderr, "  %-16s %10s %10s %10s\n", name, current, speed, acceleration);
  }
}
#endif

void dump_scope_level_table(const env_slot_scan &scan)
{
  if (scan.active_values_by_scope_level.empty())
    return;

  std::fprintf(stderr, "[napi-lifetime-scopes]\n");
  std::fprintf(stderr, "  %-16s %10s %10s %10s\n", "level", "x[i-1]", "speed", "accel");
  for (const auto &[level, count] : scan.active_values_by_scope_level)
  {
    counter_trend trend = observe_counter(std::string("scope.level.") + std::to_string(level), count);
    char current[32];
    char speed[32];
    char acceleration[32];
    format_center_value(current, sizeof(current), trend);
    format_delta_value(speed, sizeof(speed), trend, false);
    format_delta_value(acceleration, sizeof(acceleration), trend, true);
    std::fprintf(stderr, "  %-16zu %10s %10s %10s\n", level, current, speed, acceleration);
  }
}

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
struct named_count
{
  std::string name;
  size_t count = 0;
};

struct named_dual_count
{
  std::string name;
  size_t values = 0;
  size_t refs = 0;
};

void format_trend_columns(const counter_trend &trend,
                          char *current,
                          size_t current_size,
                          char *speed,
                          size_t speed_size,
                          char *acceleration,
                          size_t acceleration_size)
{
  format_center_value(current, current_size, trend);
  format_delta_value(speed, speed_size, trend, false);
  format_delta_value(acceleration, acceleration_size, trend, true);
}

void dump_string_entries(const std::vector<string_symbol_entry> &entries)
{
  size_t singular_count = 0;
  std::unordered_map<std::string, size_t> counts;
  for (const auto &entry : entries)
  {
    if (entry.tag != JS_TAG_STRING && entry.tag != JS_TAG_STRING_ROPE)
      continue;
    counts[entry.value] += entry.count;
  }

  std::vector<named_count> sorted;
  sorted.reserve(counts.size());
  for (const auto &[value, count] : counts)
    sorted.push_back({value, count});

  std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
    if (left.count != right.count)
      return left.count > right.count;
    return left.name < right.name;
  });

  if (sorted.empty())
    return;

  std::fprintf(stderr, "[napi-lifetime-strings]\n");
  std::fprintf(stderr, "  %-36s %10s %10s %10s\n", "string", "x[i-1]", "speed", "accel");
  for (const auto &entry : sorted)
  {
    if (entry.count < 2)
    {
      ++singular_count;
      continue;
    }

    counter_trend trend =
        observe_counter(std::string("string.napi_value.") + entry.name, entry.count);
    char current[32];
    char speed[32];
    char acceleration[32];
    format_trend_columns(trend,
                         current,
                         sizeof(current),
                         speed,
                         sizeof(speed),
                         acceleration,
                         sizeof(acceleration));
    std::fprintf(stderr,
                 "  %-36s %10s %10s %10s\n",
                 entry.name.c_str(),
                 current,
                 speed,
                 acceleration);
  }
  if (singular_count != 0)
    dump_metric_row("count == 1", singular_count, "string.napi_value.count_eq_1");
}

std::unordered_map<std::string, size_t> object_type_counts(
    const std::vector<object_type_entry> &entries)
{
  std::unordered_map<std::string, size_t> counts;
  for (const auto &entry : entries)
    counts[entry.prototype_name] += entry.count;
  return counts;
}

void dump_object_type_entries(const std::vector<object_type_entry> &value_entries,
                              const std::vector<object_type_entry> &ref_entries)
{
  size_t singular_value_count = 0;
  size_t singular_ref_count = 0;
  std::unordered_map<std::string, size_t> values = object_type_counts(value_entries);
  std::unordered_map<std::string, size_t> refs = object_type_counts(ref_entries);
  std::unordered_map<std::string, named_dual_count> combined;

  for (const auto &[name, count] : values)
  {
    combined[name].name = name;
    combined[name].values = count;
  }
  for (const auto &[name, count] : refs)
  {
    combined[name].name = name;
    combined[name].refs = count;
  }

  std::vector<named_dual_count> sorted;
  sorted.reserve(combined.size());
  for (const auto &[_, count] : combined)
    sorted.push_back(count);

  std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
    size_t left_total = left.values + left.refs;
    size_t right_total = right.values + right.refs;
    if (left_total != right_total)
      return left_total > right_total;
    if (left.values != right.values)
      return left.values > right.values;
    if (left.refs != right.refs)
      return left.refs > right.refs;
    return left.name < right.name;
  });

  if (sorted.empty())
    return;

  std::fprintf(stderr, "[napi-lifetime-objects]\n");
  std::fprintf(stderr,
               "  %-28s  %10s %10s %10s  %10s %10s %10s\n",
               "type",
               "values:x",
               "speed",
               "accel",
               "refs:x",
               "speed",
               "accel");
  for (const auto &entry : sorted)
  {
    bool has_printable_values = entry.values >= 2;
    bool has_printable_refs = entry.refs >= 2;
    if (!has_printable_values && entry.values != 0)
      ++singular_value_count;
    if (!has_printable_refs && entry.refs != 0)
      ++singular_ref_count;
    if (!has_printable_values && !has_printable_refs)
      continue;

    counter_trend value_trend =
        observe_counter(std::string("object.napi_value.") + entry.name, entry.values);
    counter_trend ref_trend =
        observe_counter(std::string("object.napi_ref.") + entry.name, entry.refs);
    char value_current[32];
    char value_speed[32];
    char value_acceleration[32];
    char ref_current[32];
    char ref_speed[32];
    char ref_acceleration[32];
    if (has_printable_values)
    {
      format_trend_columns(value_trend,
                           value_current,
                           sizeof(value_current),
                           value_speed,
                           sizeof(value_speed),
                           value_acceleration,
                           sizeof(value_acceleration));
    }
    else
    {
      std::snprintf(value_current, sizeof(value_current), "%s", "-");
      std::snprintf(value_speed, sizeof(value_speed), "%s", "-");
      std::snprintf(value_acceleration, sizeof(value_acceleration), "%s", "-");
    }

    if (has_printable_refs)
    {
      format_trend_columns(ref_trend,
                           ref_current,
                           sizeof(ref_current),
                           ref_speed,
                           sizeof(ref_speed),
                           ref_acceleration,
                           sizeof(ref_acceleration));
    }
    else
    {
      std::snprintf(ref_current, sizeof(ref_current), "%s", "-");
      std::snprintf(ref_speed, sizeof(ref_speed), "%s", "-");
      std::snprintf(ref_acceleration, sizeof(ref_acceleration), "%s", "-");
    }

    std::fprintf(stderr,
                 "  %-28s  %10s %10s %10s  %10s %10s %10s\n",
                 entry.name.c_str(),
                 value_current,
                 value_speed,
                 value_acceleration,
                 ref_current,
                 ref_speed,
                 ref_acceleration);
  }

  if (singular_value_count != 0 || singular_ref_count != 0)
  {
    counter_trend value_trend =
        observe_counter("object.napi_value.count_eq_1", singular_value_count);
    counter_trend ref_trend =
        observe_counter("object.napi_ref.count_eq_1", singular_ref_count);
    char value_current[32];
    char value_speed[32];
    char value_acceleration[32];
    char ref_current[32];
    char ref_speed[32];
    char ref_acceleration[32];
    format_trend_columns(value_trend,
                         value_current,
                         sizeof(value_current),
                         value_speed,
                         sizeof(value_speed),
                         value_acceleration,
                         sizeof(value_acceleration));
    format_trend_columns(ref_trend,
                         ref_current,
                         sizeof(ref_current),
                         ref_speed,
                         sizeof(ref_speed),
                         ref_acceleration,
                         sizeof(ref_acceleration));
    std::fprintf(stderr,
                 "  %-28s  %10s %10s %10s  %10s %10s %10s\n",
                 "count == 1",
                 value_current,
                 value_speed,
                 value_acceleration,
                 ref_current,
                 ref_speed,
                 ref_acceleration);
  }
}
#endif

void dump_stats_locked(napi_env env, bool include_string_symbol_values)
{
  std::fprintf(stderr, "NAPI LIFETIME TRACKER\n=====================\n");
  env_slot_scan scan = scan_env_slots(env);
  dump_counter_header("[napi-lifetime-slots]");
  dump_metric_row("napi_value.slots_total", scan.value_slots_total, "slots.value.total");
  dump_metric_row("napi_value.active", scan.active_values, "slots.value.active");
  dump_metric_row("napi_value.tracked_active", g_lifetime.values.active, "slots.value.tracked_active");
  dump_metric_row("napi_ref.slots_total", scan.ref_slots_total, "slots.ref.total");
  dump_metric_row("napi_ref.active", scan.active_refs, "slots.ref.active");
  dump_metric_row("napi_ref.tracked_active", g_lifetime.refs.active, "slots.ref.tracked_active");
  dump_metric_row("napi_scope.slots_total", scan.scope_slots_total, "slots.scope.total");
  dump_metric_row("napi_scope.active", scan.active_scopes, "slots.scope.active");
  dump_metric_row("napi_scope.escape_value.calls", g_lifetime.scope_escape_calls, "scope.escape.calls");
  dump_metric_row("napi_scope.escape_value.succeeded", g_lifetime.scope_escape_succeeded, "scope.escape.succeeded");
  dump_metric_row("napi_scope.escape_value.failed", g_lifetime.scope_escape_failed, "scope.escape.failed");
  dump_scope_level_table(scan);

  std::fprintf(stderr, "[napi-lifetime-types]\n");
  std::fprintf(stderr,
               "  %-33s %10s %10s %10s %10s %10s %10s\n",
               "type",
               "created",
               "released",
               "x[i-1]",
               "peak",
               "speed",
               "accel");
  dump_type_row("napi_value", g_lifetime.values);
  dump_type_row("napi_ref", g_lifetime.refs);
  dump_type_row("napi_env_cleanup_hook", g_lifetime.cleanup_hooks);
  dump_type_row("napi_deferred", g_lifetime.deferreds);
  dump_type_row("napi_external_backing_store_hint", g_lifetime.external_backing_store_hints);

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
  dump_tag_table("napi_value", g_lifetime.values.tags);
  dump_tag_table("napi_ref", g_lifetime.refs.tags);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (include_string_symbol_values)
  {
    dump_string_entries(g_lifetime.values.string_symbols);
    dump_object_type_entries(g_lifetime.values.object_types, g_lifetime.refs.object_types);
  }
#else
  (void)include_string_symbol_values;
#endif
  std::fprintf(stderr, "\n");
}

void dump_lifetime(napi_env env, const char *reason, bool include_string_symbol_values)
{
  if (reason != nullptr)
    std::fprintf(stderr, "[napi-lifetime] dump env=%p reason=%s\n", env, reason);

  std::lock_guard<std::mutex> lock(g_lifetime.mutex);
  dump_stats_locked(env, include_string_symbol_values);
}

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
void maybe_dump_periodic_stats(napi_env env)
{
  if (env == nullptr || !periodic_stats_enabled())
    return;

  int64_t now = monotonic_milliseconds();
  if (!env->should_dump_lifetime_stats(now))
    return;

  bool include_string_symbol_values = false;
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  include_string_symbol_values = env->should_dump_lifetime_string_symbol_values(now);
#endif
  dump_lifetime(env, nullptr, include_string_symbol_values);
}
#else
void maybe_dump_periodic_stats(napi_env env)
{
  (void)env;
}
#endif

void record_value_create(value_type_stats &stats,
                         const void *handle,
                         value_snapshot snapshot)
{
  std::lock_guard<std::mutex> lock(g_lifetime.mutex);
  record_create_locked(stats, handle, std::move(snapshot), add_value_snapshot);
}

napi_env record_value_release(value_type_stats &stats, const void *handle)
{
  std::lock_guard<std::mutex> lock(g_lifetime.mutex);
  return record_release_locked(stats, handle, remove_value_snapshot);
}

void record_basic_create(tracked_type_stats<basic_snapshot> &stats,
                         const void *handle,
                         basic_snapshot snapshot)
{
  std::lock_guard<std::mutex> lock(g_lifetime.mutex);
  record_create_locked(stats, handle, std::move(snapshot), add_basic_snapshot);
}

napi_env record_basic_release(tracked_type_stats<basic_snapshot> &stats,
                              const void *handle)
{
  std::lock_guard<std::mutex> lock(g_lifetime.mutex);
  return record_release_locked(stats, handle, remove_basic_snapshot);
}
} // namespace

void napi_lifetime__<napi_value__>::record_create(napi_scope__ *owner, napi_value__ *val)
{
  if (owner == nullptr || val == nullptr)
    return;

  napi_env env = owner->env();
  value_snapshot snapshot = capture_value_snapshot(env, val->get_inner());
  record_value_create(g_lifetime.values, val, std::move(snapshot));
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_value__>::record_release(napi_scope__ *owner, napi_value__ *val)
{
  (void)owner;
  napi_env env = record_value_release(g_lifetime.values, val);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_ref__>::record_create(napi_env__ *owner, napi_ref__ *val)
{
  if (owner == nullptr || val == nullptr)
    return;

  value_snapshot snapshot = capture_value_snapshot(owner, val->get_inner());
  napi_env env = snapshot.env;
  record_value_create(g_lifetime.refs, val, std::move(snapshot));
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_ref__>::record_release(napi_env__ *owner, napi_ref__ *val)
{
  (void)owner;
  napi_env env = record_value_release(g_lifetime.refs, val);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_env_cleanup_hook__>::record_create(napi_env__ *owner, napi_env_cleanup_hook__ *val)
{
  if (owner == nullptr || val == nullptr)
    return;

  basic_snapshot snapshot{owner};
  napi_env env = snapshot.env;
  record_basic_create(g_lifetime.cleanup_hooks, val, snapshot);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_env_cleanup_hook__>::record_release(napi_env__ *owner, napi_env_cleanup_hook__ *val)
{
  (void)owner;
  napi_env env = record_basic_release(g_lifetime.cleanup_hooks, val);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_deferred__>::record_create(napi_env__ *owner, napi_deferred__ *val)
{
  if (owner == nullptr || val == nullptr)
    return;

  basic_snapshot snapshot{owner};
  napi_env env = snapshot.env;
  record_basic_create(g_lifetime.deferreds, val, snapshot);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_deferred__>::record_release(napi_env__ *owner, napi_deferred__ *val)
{
  (void)owner;
  napi_env env = record_basic_release(g_lifetime.deferreds, val);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_external_backing_store_hint__>::record_create(
    napi_env__ *owner,
    napi_external_backing_store_hint__ *val)
{
  if (owner == nullptr || val == nullptr)
    return;

  basic_snapshot snapshot{owner};
  napi_env env = snapshot.env;
  record_basic_create(g_lifetime.external_backing_store_hints, val, snapshot);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime__<napi_external_backing_store_hint__>::record_release(
    napi_env__ *owner,
    napi_external_backing_store_hint__ *val)
{
  (void)owner;
  napi_env env = record_basic_release(g_lifetime.external_backing_store_hints, val);
  maybe_dump_periodic_stats(env);
}

void napi_lifetime_tracker__::record_scope_escape(napi_env env, bool succeeded)
{
  {
    std::lock_guard<std::mutex> lock(g_lifetime.mutex);
    ++g_lifetime.scope_escape_calls;
    if (succeeded)
      ++g_lifetime.scope_escape_succeeded;
    else
      ++g_lifetime.scope_escape_failed;
  }
  maybe_dump_periodic_stats(env);
}

void napi_lifetime_tracker__::dump(napi_env env, const char *reason)
{
  if (env == nullptr || (!enabled() && !periodic_stats_enabled()))
    return;

  dump_lifetime(env, reason, true);
}
} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(napi_env env, const char *reason)
{
  quickjs::detail::napi_lifetime_tracker__::dump(env, reason);
}
