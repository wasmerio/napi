#ifndef NAPI_QUICKJS_LIFETIME_MACROS_H_
#define NAPI_QUICKJS_LIFETIME_MACROS_H_

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
#include "internal/napi_lifetime_tracker.h"

#define NAPI_QUICKJS_LIFETIME_RECORD(action, kind, ptr, env) \
  quickjs::detail::napi_lifetime_tracker__::record_##action( \
      quickjs::detail::napi_lifetime_kind::kind, ptr, env)

#define NAPI_QUICKJS_LIFETIME_DUMP(reason) \
  quickjs::detail::napi_lifetime_tracker__::dump(reason)
#else
#define NAPI_QUICKJS_LIFETIME_RECORD(action, kind, ptr, env) ((void)0)
#define NAPI_QUICKJS_LIFETIME_DUMP(reason) ((void)0)
#endif

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
#define NAPI_QUICKJS_LIFETIME_SLOT_DELTA(kind, total_delta, active_delta) \
  quickjs::detail::napi_lifetime_tracker__::record_allocator_slot_delta( \
      kind, total_delta, active_delta)
#else
#define NAPI_QUICKJS_LIFETIME_SLOT_DELTA(kind, total_delta, active_delta) ((void)0)
#endif

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_TAG_STATS)
#define NAPI_QUICKJS_LIFETIME_TAG_DELTA(kind, scope_index, tag, active_delta) \
  quickjs::detail::napi_lifetime_tracker__::record_value_tag_delta( \
      quickjs::detail::napi_lifetime_tag_owner_kind::kind, scope_index, tag, active_delta)
#else
#define NAPI_QUICKJS_LIFETIME_TAG_DELTA(kind, scope_index, tag, active_delta) ((void)0)
#endif

#endif // NAPI_QUICKJS_LIFETIME_MACROS_H_
