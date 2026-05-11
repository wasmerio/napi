#include "internal/napi_lifetime_tracker.h"

#include "internal/napi_env.h"
#include "internal/napi_ref.h"
#include "internal/napi_scope.h"
#include "internal/napi_value.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
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

    void count_tag(tag_counters &counters, int tag)
    {
      ++counters.slots[tag_bucket_index(tag)];
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
                                 std::string value)
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
      entry.value = std::move(value);
      entry.count = 1;
      entries.push_back(std::move(entry));
    }

    void capture_string_symbol_value(napi_env env,
                                     JSValueConst value,
                                     int tag,
                                     std::vector<string_symbol_entry> &entries)
    {
      if (env == nullptr || env->context() == nullptr)
        return;

      if (tag != JS_TAG_STRING && tag != JS_TAG_STRING_ROPE)
        return;

      JSContext *ctx = env->context();
      size_t text_length = 0;
      const char *text = JS_ToCStringLen(ctx, &text_length, value);

      if (text != nullptr)
      {
        add_string_symbol_entry(entries, tag, escaped_value_fragment(text, text_length));
        JS_FreeCString(ctx, text);
      }
    }
#endif

    struct scope_scan
    {
      size_t scope_index = 0;
      size_t value_slots_total = 0;
      size_t active_values = 0;
      size_t ref_slots_total = 0;
      size_t active_refs = 0;
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
      tag_counters value_tags;
      tag_counters ref_tags;
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
      std::vector<string_symbol_entry> value_strings_symbols;
      std::vector<string_symbol_entry> ref_strings_symbols;
#endif
    };

    struct env_scan
    {
      size_t value_slots_total = 0;
      size_t active_values = 0;
      size_t ref_slots_total = 0;
      size_t active_refs = 0;
      size_t scope_slots_total = 0;
      size_t active_scopes = 0;
      std::vector<scope_scan> scopes;
    };

    void scan_scope(napi_env env, const napi_scope__ &scope, env_scan &scan)
    {
      scope_scan scope_result;
      scope_result.scope_index = scope.index();
      scope_result.value_slots_total = scope.value_storage_slot_count();
      scope_result.active_values = scope.active_value_count();
      scope_result.ref_slots_total = scope.ref_storage_slot_count();
      scope_result.active_refs = scope.active_ref_count();

      scan.value_slots_total += scope_result.value_slots_total;
      scan.active_values += scope_result.active_values;
      scan.ref_slots_total += scope_result.ref_slots_total;
      scan.active_refs += scope_result.active_refs;

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS) || \
    defined(NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP)
      scope.for_each_active_value([&](const napi_value__ &slot) {
        int tag = JS_VALUE_GET_NORM_TAG(slot.get_inner());
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
        count_tag(scope_result.value_tags, tag);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
        capture_string_symbol_value(env, slot.get_inner(), tag, scope_result.value_strings_symbols);
#endif
      });

      scope.for_each_active_ref([&](const napi_ref__ &slot) {
        int tag = JS_VALUE_GET_NORM_TAG(slot.get_inner());
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
        count_tag(scope_result.ref_tags, tag);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
        capture_string_symbol_value(env, slot.get_inner(), tag, scope_result.ref_strings_symbols);
#endif
      });
#endif

      scan.scopes.push_back(std::move(scope_result));
    }

    env_scan scan_env(napi_env env)
    {
      env_scan scan;
      if (env == nullptr)
        return scan;

      scan.scope_slots_total = env->scope_storage_slot_count();
      scan.active_scopes = env->active_scope_count();
      env->for_each_active_scope([&](const napi_scope__ &scope) {
        scan_scope(env, scope, scan);
      });
      return scan;
    }

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
    void dump_tag_line(size_t scope_index, const char *label, const tag_counters &counters)
    {
      if (!has_tags(counters))
        return;

      std::fprintf(stderr, "[napi-lifetime-tags] scope=%zu %s", scope_index, label);
      for (size_t i = 0; i < k_tag_bucket_count; ++i)
      {
        size_t count = counters.slots[i];
        if (count != 0)
          std::fprintf(stderr, " %s=%zu", tag_bucket_name(i), count);
      }
      std::fprintf(stderr, "\n");
    }
#endif

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
    void dump_string_symbol_entries(size_t scope_index,
                                    const char *label,
                                    const std::vector<string_symbol_entry> &entries)
    {
      size_t singular_count = 0;
      for (const auto &entry : entries)
      {
        if (entry.count == 1)
        {
          ++singular_count;
          continue;
        }

        std::fprintf(stderr,
                     "[napi-lifetime-values] scope=%zu %s tag=%s count=%zu value=\"%s\"\n",
                     scope_index,
                     label,
                     tag_bucket_name(tag_bucket_index(entry.tag)),
                     entry.count,
                     entry.value.c_str());
      }
      std::fprintf(stderr,
                   "[napi-lifetime-values] scope=%zu %s singular_string_count=%zu\n",
                   scope_index,
                   label,
                   singular_count);
    }
#endif

    void dump_scan(napi_env env, const char *reason, bool include_string_symbol_values)
    {
      if (reason != nullptr)
        std::fprintf(stderr, "[napi-lifetime] dump env=%p reason=%s\n", env, reason);

      env_scan scan = scan_env(env);
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

      for (const auto &scope : scan.scopes)
      {
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS
        dump_tag_line(scope.scope_index, "napi_value", scope.value_tags);
        dump_tag_line(scope.scope_index, "napi_ref", scope.ref_tags);
#endif
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
        if (include_string_symbol_values)
        {
          dump_string_symbol_entries(
              scope.scope_index, "napi_value", scope.value_strings_symbols);
          dump_string_symbol_entries(
              scope.scope_index, "napi_ref", scope.ref_strings_symbols);
        }
#else
        (void)include_string_symbol_values;
#endif
      }
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
      dump_scan(env, nullptr, include_string_symbol_values);
    }
#endif
  } // namespace

  void napi_lifetime_tracker__::maybe_dump(napi_env env)
  {
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
    maybe_dump_periodic_stats(env);
#else
    (void)env;
#endif
  }

  void napi_lifetime_tracker__::dump(napi_env env, const char *reason)
  {
    if (env == nullptr || (!enabled() && !periodic_stats_enabled()))
      return;

    dump_scan(env, reason, true);
  }
} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(napi_env env, const char *reason)
{
  quickjs::detail::napi_lifetime_tracker__::dump(env, reason);
}
