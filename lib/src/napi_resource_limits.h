#ifndef NAPI_PRIVATE_RESOURCE_LIMITS_H_
#define NAPI_PRIVATE_RESOURCE_LIMITS_H_

#include "js_native_api.h"

// Host-only configuration, not a guest import or public module API.
#ifdef __cplusplus
extern "C" {
#endif
napi_status NAPI_CDECL snapi_private_set_resource_limits(napi_env env, int enabled);
#ifdef __cplusplus
}
#endif

#endif
