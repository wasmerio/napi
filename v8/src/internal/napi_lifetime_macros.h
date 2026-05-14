#ifndef NAPI_V8_LIFETIME_MACROS_H_
#define NAPI_V8_LIFETIME_MACROS_H_

#ifdef NAPI_V8_ENABLE_LIFETIME_TRACKER
#include "internal/napi_lifetime_tracker.h"
#define NAPI_V8_LIFETIME_DUMP(env, reason) \
  v8impl::detail::napi_lifetime_tracker__::dump(env, reason)
#else
#define NAPI_V8_LIFETIME_DUMP(env, reason) ((void)0)
#endif

#endif  // NAPI_V8_LIFETIME_MACROS_H_
