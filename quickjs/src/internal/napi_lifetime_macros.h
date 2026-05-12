#ifndef NAPI_QUICKJS_LIFETIME_MACROS_H_
#define NAPI_QUICKJS_LIFETIME_MACROS_H_

#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
#include "internal/napi_lifetime_tracker.h"

#define NAPI_QUICKJS_LIFETIME_DUMP(env, reason) \
  quickjs::detail::napi_lifetime_tracker__::dump(env, reason)
#else
#define NAPI_QUICKJS_LIFETIME_DUMP(env, reason) ((void)0)
#endif

#endif // NAPI_QUICKJS_LIFETIME_MACROS_H_
