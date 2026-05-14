#include "internal/napi_lifetime_tracker.h"

#include "internal/napi_ref__.h"
#include "internal/napi_v8_env.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace v8impl::detail {
namespace {

bool enabled() {
  const char* value = std::getenv("EDGE_TRACE_NAPI_LIFETIME");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool periodic_stats_enabled() {
  const char* value = std::getenv("EDGE_TRACE_NAPI_LIFETIME_STATS");
  if (value != nullptr && value[0] != '\0') {
    return value[0] != '0';
  }
  return enabled();
}

int64_t monotonic_milliseconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

#ifdef NAPI_V8_ENABLE_LIFETIME_TRACKER

struct type_stats {
  std::size_t created = 0;
  std::size_t released = 0;
  std::size_t active = 0;
  std::size_t peak = 0;
};

#if defined(NAPI_V8_ENABLE_LIFETIME_TAG_STATS) || \
    defined(NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP)
enum class value_tag : std::size_t {
  kBigInt,
  kSymbol,
  kString,
  kFunction,
  kObject,
  kExternal,
  kArray,
  kArrayBuffer,
  kTypedArray,
  kDataView,
  kPromise,
  kInt,
  kUint32,
  kBool,
  kNull,
  kUndefined,
  kNumber,
  kUnknown,
  kCount,
};

constexpr std::size_t k_tag_bucket_count =
    static_cast<std::size_t>(value_tag::kCount);

const char* tag_bucket_name(std::size_t index) {
  switch (static_cast<value_tag>(index)) {
    case value_tag::kBigInt:
      return "big_int";
    case value_tag::kSymbol:
      return "symbol";
    case value_tag::kString:
      return "string";
    case value_tag::kFunction:
      return "function";
    case value_tag::kObject:
      return "object";
    case value_tag::kExternal:
      return "external";
    case value_tag::kArray:
      return "array";
    case value_tag::kArrayBuffer:
      return "array_buffer";
    case value_tag::kTypedArray:
      return "typed_array";
    case value_tag::kDataView:
      return "data_view";
    case value_tag::kPromise:
      return "promise";
    case value_tag::kInt:
      return "int";
    case value_tag::kUint32:
      return "uint32";
    case value_tag::kBool:
      return "bool";
    case value_tag::kNull:
      return "null";
    case value_tag::kUndefined:
      return "undefined";
    case value_tag::kNumber:
      return "float64";
    default:
      return "unknown";
  }
}

struct tag_counters {
  std::array<std::size_t, k_tag_bucket_count> slots{};
};

void add_tag(tag_counters& counters, value_tag tag) {
  ++counters.slots[static_cast<std::size_t>(tag)];
}

void remove_tag(tag_counters& counters, value_tag tag) {
  std::size_t& slot = counters.slots[static_cast<std::size_t>(tag)];
  if (slot > 0) {
    --slot;
  }
}

bool has_tags(const tag_counters& counters) {
  return std::any_of(counters.slots.begin(), counters.slots.end(), [](auto count) {
    return count != 0;
  });
}

value_tag classify_value(v8::Local<v8::Value> value) {
  if (value.IsEmpty()) return value_tag::kUnknown;
  if (value->IsUndefined()) return value_tag::kUndefined;
  if (value->IsNull()) return value_tag::kNull;
  if (value->IsBoolean()) return value_tag::kBool;
  if (value->IsInt32()) return value_tag::kInt;
  if (value->IsUint32()) return value_tag::kUint32;
  if (value->IsNumber()) return value_tag::kNumber;
  if (value->IsBigInt()) return value_tag::kBigInt;
  if (value->IsSymbol()) return value_tag::kSymbol;
  if (value->IsString()) return value_tag::kString;
  if (value->IsFunction()) return value_tag::kFunction;
  if (value->IsExternal()) return value_tag::kExternal;
  if (value->IsArray()) return value_tag::kArray;
  if (value->IsArrayBuffer()) return value_tag::kArrayBuffer;
  if (value->IsTypedArray()) return value_tag::kTypedArray;
  if (value->IsDataView()) return value_tag::kDataView;
  if (value->IsPromise()) return value_tag::kPromise;
  if (value->IsObject()) return value_tag::kObject;
  return value_tag::kUnknown;
}
#endif

#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
constexpr std::size_t k_value_dump_max_bytes = 240;

struct string_symbol_entry {
  value_tag tag = value_tag::kUnknown;
  std::string value;
  std::size_t count = 0;
};

struct object_type_entry {
  std::string prototype_name;
  std::size_t count = 0;
};

void append_hex_escape(std::string& out, unsigned char c) {
  constexpr char hex[] = "0123456789abcdef";
  out.push_back('\\');
  out.push_back('x');
  out.push_back(hex[(c >> 4) & 0x0f]);
  out.push_back(hex[c & 0x0f]);
}

std::string escaped_value_fragment(const char* value, std::size_t value_length) {
  std::size_t limit =
      value_length < k_value_dump_max_bytes ? value_length : k_value_dump_max_bytes;
  std::string out;
  out.reserve(limit + 3);
  for (std::size_t i = 0; i < limit; ++i) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c) {
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
        if (c >= 0x20 && c <= 0x7e) {
          out.push_back(static_cast<char>(c));
        } else {
          append_hex_escape(out, c);
        }
        break;
    }
  }
  if (value_length > limit) {
    out += "...";
  }
  return out;
}

std::string escaped_v8_string(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  if (*utf8 == nullptr) {
    return {};
  }
  return escaped_value_fragment(*utf8, static_cast<std::size_t>(utf8.length()));
}

std::string object_prototype_name(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject()) {
    return {};
  }

  v8::Local<v8::String> name =
      value.As<v8::Object>()->GetConstructorName();
  std::string result = escaped_v8_string(isolate, name);
  return result.empty() ? "<object>" : result;
}

void add_string_symbol_entry(std::vector<string_symbol_entry>& entries,
                             value_tag tag,
                             const std::string& value) {
  for (auto& entry : entries) {
    if (entry.tag == tag && entry.value == value) {
      ++entry.count;
      return;
    }
  }
  entries.push_back({tag, value, 1});
}

void remove_string_symbol_entry(std::vector<string_symbol_entry>& entries,
                                value_tag tag,
                                const std::string& value) {
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->tag == tag && it->value == value) {
      if (it->count > 1) {
        --it->count;
      } else {
        entries.erase(it);
      }
      return;
    }
  }
}

void add_object_type_entry(std::vector<object_type_entry>& entries,
                           const std::string& prototype_name) {
  for (auto& entry : entries) {
    if (entry.prototype_name == prototype_name) {
      ++entry.count;
      return;
    }
  }
  entries.push_back({prototype_name, 1});
}

void remove_object_type_entry(std::vector<object_type_entry>& entries,
                              const std::string& prototype_name) {
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->prototype_name == prototype_name) {
      if (it->count > 1) {
        --it->count;
      } else {
        entries.erase(it);
      }
      return;
    }
  }
}
#endif

struct value_snapshot {
  napi_env env = nullptr;
  const void* scope_key = nullptr;
  std::size_t scope_level = 0;
#if defined(NAPI_V8_ENABLE_LIFETIME_TAG_STATS) || \
    defined(NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP)
  value_tag tag = value_tag::kUnknown;
#endif
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  bool has_string_symbol = false;
  std::string string_symbol_value;
  bool has_object_type = false;
  std::string object_type;
#endif
};

struct value_type_stats : type_stats {
#ifdef NAPI_V8_ENABLE_LIFETIME_TAG_STATS
  tag_counters tags;
#endif
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  std::vector<string_symbol_entry> string_symbols;
  std::vector<object_type_entry> object_types;
#endif
};

struct live_record {
  napi_env env = nullptr;
  const char* type_name = "unknown";
  std::size_t order = 0;
  bool has_value_snapshot = false;
  value_snapshot snapshot;
};

struct lifetime_state {
  std::mutex mutex;
  std::unordered_map<void*, live_record> live;
  std::unordered_map<std::string, type_stats> types;
  value_type_stats values;
  value_type_stats refs;
  std::unordered_map<const void*, std::vector<value_snapshot>> scope_values;
  std::size_t next_order = 1;
  std::size_t scope_escape_calls = 0;
  std::size_t scope_escape_succeeded = 0;
  std::size_t scope_escape_failed = 0;
  std::unordered_map<std::string, std::array<std::size_t, 3>> counter_history;
  std::unordered_map<std::string, std::size_t> counter_history_size;
  std::unordered_map<std::string, std::size_t> counter_history_next;
};

lifetime_state g_lifetime;

struct counter_trend {
  bool available = false;
  std::size_t current = 0;
  long long speed = 0;
  long long acceleration = 0;
};

counter_trend observe_counter(const std::string& key, std::size_t value) {
  counter_trend trend;
  std::size_t& history_size = g_lifetime.counter_history_size[key];
  std::size_t& next = g_lifetime.counter_history_next[key];
  auto& history = g_lifetime.counter_history[key];

  history[next] = value;
  next = (next + 1) % history.size();
  if (history_size < history.size()) {
    ++history_size;
  }

  if (history_size == history.size()) {
    std::size_t i_minus_2_index = next;
    std::size_t i_minus_1_index = (next + 1) % history.size();
    std::size_t i_index = (next + 2) % history.size();
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

void format_center_value(char* buffer,
                         std::size_t buffer_size,
                         const counter_trend& trend) {
  if (!trend.available) {
    std::snprintf(buffer, buffer_size, "%s", "-");
    return;
  }
  std::snprintf(buffer, buffer_size, "%zu", trend.current);
}

void format_delta_value(char* buffer,
                        std::size_t buffer_size,
                        const counter_trend& trend,
                        bool acceleration) {
  if (!trend.available) {
    std::snprintf(buffer, buffer_size, "%s", "-");
    return;
  }
  std::snprintf(buffer,
                buffer_size,
                "%lld",
                acceleration ? trend.acceleration : trend.speed);
}

void dump_metric_row(const char* metric,
                     std::size_t value,
                     const std::string& key) {
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

void dump_counter_header(const char* title) {
  std::fprintf(stderr, "%s\n", title);
  std::fprintf(stderr,
               "  %-36s %10s %10s %10s\n",
               "metric",
               "x[i-1]",
               "speed",
               "accel");
}

bool is_napi_ref_type(const char* type_name) {
  return type_name != nullptr && std::strcmp(type_name, "napi_ref") == 0;
}

const void* value_scope_key(napi_env env, std::size_t parent_scope_depth) {
  if (env == nullptr) {
    return nullptr;
  }
  if (env->open_handle_scope_stack.size() <= parent_scope_depth) {
    return env;
  }
  return env->open_handle_scope_stack[env->open_handle_scope_stack.size() - 1 -
                                      parent_scope_depth];
}

std::size_t value_scope_level(napi_env env, std::size_t parent_scope_depth) {
  if (env == nullptr || env->open_handle_scope_stack.size() <= parent_scope_depth) {
    return 0;
  }
  return env->open_handle_scope_stack.size() - parent_scope_depth;
}

value_snapshot capture_value_snapshot(napi_env env,
                                      v8::Local<v8::Value> local,
                                      std::size_t parent_scope_depth) {
  value_snapshot snapshot;
  snapshot.env = env;
  snapshot.scope_key = value_scope_key(env, parent_scope_depth);
  snapshot.scope_level = value_scope_level(env, parent_scope_depth);
#if defined(NAPI_V8_ENABLE_LIFETIME_TAG_STATS) || \
    defined(NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP)
  if (env == nullptr || env->isolate == nullptr || local.IsEmpty()) {
    return snapshot;
  }
  snapshot.tag = classify_value(local);
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (snapshot.tag == value_tag::kString) {
    snapshot.string_symbol_value = escaped_v8_string(env->isolate, local);
    snapshot.has_string_symbol = !snapshot.string_symbol_value.empty();
  } else if (snapshot.tag == value_tag::kSymbol) {
    v8::Local<v8::Value> description =
        local.As<v8::Symbol>()->Description(env->isolate);
    if (!description.IsEmpty() && description->IsString()) {
      snapshot.string_symbol_value = escaped_v8_string(env->isolate, description);
      snapshot.has_string_symbol = !snapshot.string_symbol_value.empty();
    }
  }

  if (local->IsObject()) {
    snapshot.object_type = object_prototype_name(env->isolate, local);
    snapshot.has_object_type = !snapshot.object_type.empty();
  }
#endif
#endif
  return snapshot;
}

value_snapshot capture_ref_snapshot(napi_env env, void* value) {
  auto* ref = static_cast<napi_ref__*>(value);
  if (env == nullptr || env->isolate == nullptr || ref == nullptr) {
    value_snapshot snapshot;
    snapshot.env = env;
    return snapshot;
  }

  v8::HandleScope handle_scope(env->isolate);
  v8::Local<v8::Value> local = ref->Get();
  if (local.IsEmpty()) {
    value_snapshot snapshot;
    snapshot.env = env;
    return snapshot;
  }
  return capture_value_snapshot(env, local, 0);
}

void add_value_snapshot(value_type_stats& stats, const value_snapshot& snapshot) {
#ifdef NAPI_V8_ENABLE_LIFETIME_TAG_STATS
  add_tag(stats.tags, snapshot.tag);
#endif
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (snapshot.has_string_symbol) {
    add_string_symbol_entry(stats.string_symbols,
                            snapshot.tag,
                            snapshot.string_symbol_value);
  }
  if (snapshot.has_object_type) {
    add_object_type_entry(stats.object_types, snapshot.object_type);
  }
#endif
}

void remove_value_snapshot(value_type_stats& stats,
                           const value_snapshot& snapshot) {
#ifdef NAPI_V8_ENABLE_LIFETIME_TAG_STATS
  remove_tag(stats.tags, snapshot.tag);
#endif
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (snapshot.has_string_symbol) {
    remove_string_symbol_entry(stats.string_symbols,
                               snapshot.tag,
                               snapshot.string_symbol_value);
  }
  if (snapshot.has_object_type) {
    remove_object_type_entry(stats.object_types, snapshot.object_type);
  }
#endif
}

void record_ref_create_locked(const value_snapshot& snapshot) {
  ++g_lifetime.refs.created;
  ++g_lifetime.refs.active;
  g_lifetime.refs.peak = std::max(g_lifetime.refs.peak, g_lifetime.refs.active);
  add_value_snapshot(g_lifetime.refs, snapshot);
}

void record_ref_release_locked(const value_snapshot& snapshot) {
  remove_value_snapshot(g_lifetime.refs, snapshot);
  ++g_lifetime.refs.released;
  if (g_lifetime.refs.active > 0) {
    --g_lifetime.refs.active;
  }
}

void record_value_create_locked(napi_env env,
                                v8::Local<v8::Value> value,
                                std::size_t parent_scope_depth) {
  if (env == nullptr || value.IsEmpty()) {
    return;
  }

  value_snapshot snapshot =
      capture_value_snapshot(env, value, parent_scope_depth);
  ++g_lifetime.values.created;
  ++g_lifetime.values.active;
  g_lifetime.values.peak =
      std::max(g_lifetime.values.peak, g_lifetime.values.active);
  add_value_snapshot(g_lifetime.values, snapshot);
  g_lifetime.scope_values[snapshot.scope_key].push_back(std::move(snapshot));
}

void record_scope_values_release_locked(napi_env env, const void* scope) {
  const void* key = scope == nullptr ? static_cast<const void*>(env) : scope;
  auto it = g_lifetime.scope_values.find(key);
  if (it == g_lifetime.scope_values.end()) {
    return;
  }

  for (const auto& snapshot : it->second) {
    remove_value_snapshot(g_lifetime.values, snapshot);
    ++g_lifetime.values.released;
    if (g_lifetime.values.active > 0) {
      --g_lifetime.values.active;
    }
  }
  g_lifetime.scope_values.erase(it);
}

void record_create_locked(napi_env env, void* value, const char* type_name) {
  if (value == nullptr) return;

  const char* effective_type = type_name == nullptr ? "unknown" : type_name;
  auto existing = g_lifetime.live.find(value);
  if (existing != g_lifetime.live.end()) {
    auto& old_stats = g_lifetime.types[existing->second.type_name];
    if (old_stats.active > 0) {
      --old_stats.active;
    }
    if (existing->second.has_value_snapshot) {
      record_ref_release_locked(existing->second.snapshot);
    }
    g_lifetime.live.erase(existing);
  }

  auto& stats = g_lifetime.types[effective_type];
  ++stats.created;
  ++stats.active;
  stats.peak = std::max(stats.peak, stats.active);

  live_record record;
  record.env = env;
  record.type_name = effective_type;
  record.order = g_lifetime.next_order++;
  if (is_napi_ref_type(effective_type)) {
    record.has_value_snapshot = true;
    record.snapshot = capture_ref_snapshot(env, value);
    record_ref_create_locked(record.snapshot);
  }
  g_lifetime.live.emplace(value, std::move(record));
}

void record_release_locked(void* value, const char* type_name) {
  if (value == nullptr) return;

  auto live_it = g_lifetime.live.find(value);
  const char* effective_type = type_name == nullptr ? "unknown" : type_name;
  if (live_it != g_lifetime.live.end()) {
    effective_type = live_it->second.type_name;
    if (live_it->second.has_value_snapshot) {
      record_ref_release_locked(live_it->second.snapshot);
    }
    g_lifetime.live.erase(live_it);
  }

  auto& stats = g_lifetime.types[effective_type];
  ++stats.released;
  if (stats.active > 0) {
    --stats.active;
  }
}

struct env_slot_scan {
  std::size_t value_slots_total = 0;
  std::size_t active_values = 0;
  std::size_t ref_slots_total = 0;
  std::size_t active_refs = 0;
  std::size_t scope_slots_total = 0;
  std::size_t active_scopes = 0;
  std::vector<std::pair<std::size_t, std::size_t>> active_values_by_scope_level;
};

type_stats type_stats_or_empty(const char* type_name) {
  auto it = g_lifetime.types.find(type_name);
  return it == g_lifetime.types.end() ? type_stats{} : it->second;
}

env_slot_scan scan_env_slots(napi_env env) {
  env_slot_scan scan;
  if (env == nullptr) {
    return scan;
  }

  scan.value_slots_total = g_lifetime.values.active;
  scan.active_values = g_lifetime.values.active;
  std::unordered_map<std::size_t, std::size_t> active_values_by_scope_level;
  for (const auto& [_, snapshots] : g_lifetime.scope_values) {
    for (const auto& snapshot : snapshots) {
      if (snapshot.env == env) {
        active_values_by_scope_level[snapshot.scope_level]++;
      }
    }
  }
  scan.active_values_by_scope_level.reserve(active_values_by_scope_level.size());
  for (const auto& [level, count] : active_values_by_scope_level) {
    scan.active_values_by_scope_level.push_back({level, count});
  }
  std::sort(scan.active_values_by_scope_level.begin(),
            scan.active_values_by_scope_level.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  type_stats ref_stats = type_stats_or_empty("napi_ref");
  type_stats handle_scope_stats = type_stats_or_empty("napi_handle_scope");
  type_stats escapable_scope_stats =
      type_stats_or_empty("napi_escapable_handle_scope");
  scan.ref_slots_total = ref_stats.created;
  scan.active_refs = ref_stats.active;
  scan.scope_slots_total =
      handle_scope_stats.created + escapable_scope_stats.created;
  scan.active_scopes =
      handle_scope_stats.active + escapable_scope_stats.active;
  return scan;
}

template <typename Stats>
void dump_type_row(const char* label, const Stats& stats) {
  counter_trend trend =
      observe_counter(std::string("type.") + label + ".active", stats.active);
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

#ifdef NAPI_V8_ENABLE_LIFETIME_TAG_STATS
void dump_tag_table(const char* label, const tag_counters& counters) {
  if (!has_tags(counters)) {
    return;
  }

  std::fprintf(stderr, "[napi-lifetime-tags] owner=%s\n", label);
  std::fprintf(stderr,
               "  %-16s %10s %10s %10s\n",
               "tag",
               "x[i-1]",
               "speed",
               "accel");
  for (std::size_t i = 0; i < k_tag_bucket_count; ++i) {
    std::size_t count = counters.slots[i];
    if (count == 0) {
      continue;
    }
    const char* name = tag_bucket_name(i);
    counter_trend trend =
        observe_counter(std::string("tag.") + label + "." + name, count);
    char current[32];
    char speed[32];
    char acceleration[32];
    format_center_value(current, sizeof(current), trend);
    format_delta_value(speed, sizeof(speed), trend, false);
    format_delta_value(acceleration, sizeof(acceleration), trend, true);
    std::fprintf(stderr,
                 "  %-16s %10s %10s %10s\n",
                 name,
                 current,
                 speed,
                 acceleration);
  }
}

void dump_scope_value_tag_tables(napi_env env) {
  std::map<std::size_t, tag_counters> by_scope_level;
  for (const auto& [_, snapshots] : g_lifetime.scope_values) {
    for (const auto& snapshot : snapshots) {
      if (snapshot.env == env) {
        add_tag(by_scope_level[snapshot.scope_level], snapshot.tag);
      }
    }
  }

  for (const auto& [level, counters] : by_scope_level) {
    if (!has_tags(counters)) {
      continue;
    }
    std::fprintf(stderr,
                 "[napi-lifetime-scope-values] scope_level=%zu owner=napi_value\n",
                 level);
    std::fprintf(stderr,
                 "  %-16s %10s %10s %10s\n",
                 "tag",
                 "x[i-1]",
                 "speed",
                 "accel");
    for (std::size_t i = 0; i < k_tag_bucket_count; ++i) {
      std::size_t count = counters.slots[i];
      if (count == 0) {
        continue;
      }
      const char* name = tag_bucket_name(i);
      counter_trend trend = observe_counter(std::string("scope.tag.") +
                                            std::to_string(level) + "." + name,
                                            count);
      char current[32];
      char speed[32];
      char acceleration[32];
      format_center_value(current, sizeof(current), trend);
      format_delta_value(speed, sizeof(speed), trend, false);
      format_delta_value(acceleration, sizeof(acceleration), trend, true);
      std::fprintf(stderr,
                   "  %-16s %10s %10s %10s\n",
                   name,
                   current,
                   speed,
                   acceleration);
    }
  }
}
#endif

void dump_scope_level_table(const env_slot_scan& scan) {
  if (scan.active_values_by_scope_level.empty()) {
    return;
  }

  std::fprintf(stderr, "[napi-lifetime-scopes]\n");
  std::fprintf(stderr,
               "  %-16s %10s %10s %10s\n",
               "level",
               "x[i-1]",
               "speed",
               "accel");
  for (const auto& [level, count] : scan.active_values_by_scope_level) {
    counter_trend trend =
        observe_counter(std::string("scope.level.") + std::to_string(level),
                        count);
    char current[32];
    char speed[32];
    char acceleration[32];
    format_center_value(current, sizeof(current), trend);
    format_delta_value(speed, sizeof(speed), trend, false);
    format_delta_value(acceleration, sizeof(acceleration), trend, true);
    std::fprintf(stderr,
                 "  %-16zu %10s %10s %10s\n",
                 level,
                 current,
                 speed,
                 acceleration);
  }
}

#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
struct named_count {
  std::string name;
  std::size_t count = 0;
};

struct named_dual_count {
  std::string name;
  std::size_t values = 0;
  std::size_t refs = 0;
};

void format_trend_columns(const counter_trend& trend,
                          char* current,
                          std::size_t current_size,
                          char* speed,
                          std::size_t speed_size,
                          char* acceleration,
                          std::size_t acceleration_size) {
  format_center_value(current, current_size, trend);
  format_delta_value(speed, speed_size, trend, false);
  format_delta_value(acceleration, acceleration_size, trend, true);
}

void dump_string_entries(const std::vector<string_symbol_entry>& entries) {
  std::size_t singular_count = 0;
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto& entry : entries) {
    if (entry.tag != value_tag::kString && entry.tag != value_tag::kSymbol) {
      continue;
    }
    counts[entry.value] += entry.count;
  }

  std::vector<named_count> sorted;
  sorted.reserve(counts.size());
  for (const auto& [value, count] : counts) {
    sorted.push_back({value, count});
  }

  std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
    if (left.count != right.count) {
      return left.count > right.count;
    }
    return left.name < right.name;
  });

  if (sorted.empty()) {
    return;
  }

  std::fprintf(stderr, "[napi-lifetime-strings]\n");
  std::fprintf(stderr,
               "  %-36s %10s %10s %10s\n",
               "string",
               "x[i-1]",
               "speed",
               "accel");
  for (const auto& entry : sorted) {
    if (entry.count < 2) {
      ++singular_count;
      continue;
    }

    counter_trend trend =
        observe_counter(std::string("string.napi_ref.") + entry.name,
                        entry.count);
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
  if (singular_count != 0) {
    dump_metric_row("count == 1", singular_count, "string.napi_ref.count_eq_1");
  }
}

std::unordered_map<std::string, std::size_t> object_type_counts(
    const std::vector<object_type_entry>& entries) {
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto& entry : entries) {
    counts[entry.prototype_name] += entry.count;
  }
  return counts;
}

void dump_object_type_entries(
    const std::vector<object_type_entry>& value_entries,
    const std::vector<object_type_entry>& ref_entries) {
  std::size_t singular_value_count = 0;
  std::size_t singular_ref_count = 0;
  std::unordered_map<std::string, std::size_t> values =
      object_type_counts(value_entries);
  std::unordered_map<std::string, std::size_t> refs =
      object_type_counts(ref_entries);
  std::unordered_map<std::string, named_dual_count> combined;

  for (const auto& [name, count] : values) {
    combined[name].name = name;
    combined[name].values = count;
  }
  for (const auto& [name, count] : refs) {
    combined[name].name = name;
    combined[name].refs = count;
  }

  std::vector<named_dual_count> sorted;
  sorted.reserve(combined.size());
  for (const auto& [_, count] : combined) {
    sorted.push_back(count);
  }

  std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
    std::size_t left_total = left.values + left.refs;
    std::size_t right_total = right.values + right.refs;
    if (left_total != right_total) {
      return left_total > right_total;
    }
    if (left.values != right.values) {
      return left.values > right.values;
    }
    if (left.refs != right.refs) {
      return left.refs > right.refs;
    }
    return left.name < right.name;
  });

  if (sorted.empty()) {
    return;
  }

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
  for (const auto& entry : sorted) {
    bool has_printable_values = entry.values >= 2;
    bool has_printable_refs = entry.refs >= 2;
    if (!has_printable_values && entry.values != 0) {
      ++singular_value_count;
    }
    if (!has_printable_refs && entry.refs != 0) {
      ++singular_ref_count;
    }
    if (!has_printable_values && !has_printable_refs) {
      continue;
    }

    counter_trend value_trend =
        observe_counter(std::string("object.napi_value.") + entry.name,
                        entry.values);
    counter_trend ref_trend =
        observe_counter(std::string("object.napi_ref.") + entry.name,
                        entry.refs);
    char value_current[32];
    char value_speed[32];
    char value_acceleration[32];
    char ref_current[32];
    char ref_speed[32];
    char ref_acceleration[32];
    if (has_printable_values) {
      format_trend_columns(value_trend,
                           value_current,
                           sizeof(value_current),
                           value_speed,
                           sizeof(value_speed),
                           value_acceleration,
                           sizeof(value_acceleration));
    } else {
      std::snprintf(value_current, sizeof(value_current), "%s", "-");
      std::snprintf(value_speed, sizeof(value_speed), "%s", "-");
      std::snprintf(value_acceleration, sizeof(value_acceleration), "%s", "-");
    }

    if (has_printable_refs) {
      format_trend_columns(ref_trend,
                           ref_current,
                           sizeof(ref_current),
                           ref_speed,
                           sizeof(ref_speed),
                           ref_acceleration,
                           sizeof(ref_acceleration));
    } else {
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

  if (singular_value_count != 0 || singular_ref_count != 0) {
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

void dump_stats_locked(napi_env env, bool include_string_symbol_values) {
  std::fprintf(stderr, "NAPI LIFETIME TRACKER\n=====================\n");
  env_slot_scan scan = scan_env_slots(env);
  dump_counter_header("[napi-lifetime-slots]");
  dump_metric_row("napi_value.slots_total",
                  scan.value_slots_total,
                  "slots.value.total");
  dump_metric_row("napi_value.active", scan.active_values, "slots.value.active");
  dump_metric_row("napi_value.tracked_active",
                  g_lifetime.values.active,
                  "slots.value.tracked_active");
  dump_metric_row("napi_ref.slots_total", scan.ref_slots_total, "slots.ref.total");
  dump_metric_row("napi_ref.active", scan.active_refs, "slots.ref.active");
  dump_metric_row("napi_ref.tracked_active",
                  g_lifetime.refs.active,
                  "slots.ref.tracked_active");
  dump_metric_row("napi_scope.slots_total",
                  scan.scope_slots_total,
                  "slots.scope.total");
  dump_metric_row("napi_scope.active", scan.active_scopes, "slots.scope.active");
  dump_metric_row("napi_scope.escape_value.calls",
                  g_lifetime.scope_escape_calls,
                  "scope.escape.calls");
  dump_metric_row("napi_scope.escape_value.succeeded",
                  g_lifetime.scope_escape_succeeded,
                  "scope.escape.succeeded");
  dump_metric_row("napi_scope.escape_value.failed",
                  g_lifetime.scope_escape_failed,
                  "scope.escape.failed");
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

  std::vector<std::pair<std::string, type_stats>> rows(g_lifetime.types.begin(),
                                                       g_lifetime.types.end());
  std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  for (const auto& [type, stats] : rows) {
    dump_type_row(type.c_str(), stats);
  }

#ifdef NAPI_V8_ENABLE_LIFETIME_TAG_STATS
  dump_tag_table("napi_value", g_lifetime.values.tags);
  dump_tag_table("napi_ref", g_lifetime.refs.tags);
  dump_scope_value_tag_tables(env);
#endif
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  if (include_string_symbol_values) {
    dump_string_entries(g_lifetime.values.string_symbols);
    dump_object_type_entries(g_lifetime.values.object_types,
                             g_lifetime.refs.object_types);
  }
#else
  (void)include_string_symbol_values;
#endif
  std::fprintf(stderr, "\n");
}

void dump_summary_locked(napi_env env) {
  env_slot_scan scan = scan_env_slots(env);
  std::fprintf(stderr,
               "[napi-lifetime-stats] napi_value slots_total=%zu active=%zu "
               "napi_ref slots_total=%zu active=%zu "
               "napi_scope slots_total=%zu active=%zu\n",
               scan.value_slots_total,
               scan.active_values,
               scan.ref_slots_total,
               scan.active_refs,
               scan.scope_slots_total,
               scan.active_scopes);
}

void dump_lifetime(napi_env env,
                   const char* reason,
                   bool include_string_symbol_values) {
  if (reason != nullptr) {
    std::fprintf(stderr, "[napi-lifetime] dump env=%p reason=%s\n", env, reason);
  }

  std::lock_guard<std::mutex> lock{g_lifetime.mutex};
  dump_stats_locked(env, include_string_symbol_values);
}

void dump_lifetime_summary(napi_env env) {
  std::lock_guard<std::mutex> lock{g_lifetime.mutex};
  dump_summary_locked(env);
}

#ifdef NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS
void maybe_dump_periodic_stats(napi_env env) {
  if (env == nullptr || !periodic_stats_enabled()) {
    return;
  }

  int64_t now = monotonic_milliseconds();
  bool should_dump_summary = env->should_dump_lifetime_stats(now);
  bool should_dump_full = false;
#ifdef NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  should_dump_full = env->should_dump_lifetime_string_symbol_values(now);
#endif

  if (should_dump_full) {
    dump_lifetime(env, nullptr, true);
    return;
  }

  if (should_dump_summary) {
    dump_lifetime_summary(env);
  }
}
#else
void maybe_dump_periodic_stats(napi_env env) {
  (void)env;
}
#endif

#endif  // NAPI_V8_ENABLE_LIFETIME_TRACKER

}  // namespace

#ifdef NAPI_V8_ENABLE_LIFETIME_TRACKER
void napi_lifetime_tracker__::record_create_raw(napi_env env,
                                                void* value,
                                                const char* type_name) {
  {
    std::lock_guard<std::mutex> lock{g_lifetime.mutex};
    record_create_locked(env, value, type_name);
  }
  maybe_dump_periodic_stats(env);
}

void napi_lifetime_tracker__::record_release_raw(napi_env env,
                                                 void* value,
                                                 const char* type_name) {
  napi_env owner = env;
  {
    std::lock_guard<std::mutex> lock{g_lifetime.mutex};
    auto live_it = g_lifetime.live.find(value);
    if (owner == nullptr && live_it != g_lifetime.live.end()) {
      owner = live_it->second.env;
    }
    record_release_locked(value, type_name);
  }
  maybe_dump_periodic_stats(owner);
}

void napi_lifetime_tracker__::record_scope_escape(napi_env env, bool succeeded) {
  {
    std::lock_guard<std::mutex> lock{g_lifetime.mutex};
    ++g_lifetime.scope_escape_calls;
    if (succeeded) {
      ++g_lifetime.scope_escape_succeeded;
    } else {
      ++g_lifetime.scope_escape_failed;
    }
  }
  maybe_dump_periodic_stats(env);
}

void napi_lifetime_tracker__::record_value(napi_env env,
                                           v8::Local<v8::Value> value,
                                           std::size_t parent_scope_depth) {
  {
    std::lock_guard<std::mutex> lock{g_lifetime.mutex};
    record_value_create_locked(env, value, parent_scope_depth);
  }
  maybe_dump_periodic_stats(env);
}

void napi_lifetime_tracker__::record_scope_values_release(napi_env env,
                                                          const void* scope) {
  {
    std::lock_guard<std::mutex> lock{g_lifetime.mutex};
    record_scope_values_release_locked(env, scope);
  }
  maybe_dump_periodic_stats(env);
}
#endif

void napi_lifetime_tracker__::dump(napi_env env, const char* reason) {
#ifdef NAPI_V8_ENABLE_LIFETIME_TRACKER
  if (env == nullptr || !enabled()) return;
  dump_lifetime(env, reason, true);
#else
  (void)env;
  (void)reason;
#endif
}

}  // namespace v8impl::detail

extern "C" void napi_v8_lifetime_dump(napi_env env, const char* reason) {
  v8impl::detail::napi_lifetime_tracker__::dump(env, reason);
}
