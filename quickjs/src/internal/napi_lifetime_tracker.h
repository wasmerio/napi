#ifndef NAPI_QUICKJS_LIFETIME_TRACKER_H_
#define NAPI_QUICKJS_LIFETIME_TRACKER_H_

#include <cstddef>

namespace quickjs::detail
{

enum class napi_lifetime_kind
{
  env,
  scope,
  handle_scope,
  escapable_handle_scope,
  value,
  ref,
  callback_info,
  external_hint,
  deferred,
  cleanup_hook,
};

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
enum class napi_lifetime_slot_kind
{
  value,
  ref,
};
#endif

class napi_lifetime_tracker__
{
public:
  static void record_create(napi_lifetime_kind kind, const void *ptr, const void *env);
  static void record_destroy(napi_lifetime_kind kind, const void *ptr, const void *env);
  static void dump(const char *reason);
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS
  static void record_allocator_slot_delta(napi_lifetime_slot_kind kind,
                                          std::ptrdiff_t total_delta,
                                          std::ptrdiff_t active_delta);
#endif
};

} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(const char *reason);

#endif // NAPI_QUICKJS_LIFETIME_TRACKER_H_
