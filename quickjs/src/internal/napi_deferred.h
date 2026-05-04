#ifndef NAPI_QUICKJS_DEFERRED_H_
#define NAPI_QUICKJS_DEFERRED_H_

#include "../../../include/js_native_api.h"

#include <quickjs.h>

struct napi_deferred__
{
  static napi_deferred__ *create(napi_env env, JSValue resolve, JSValue reject);
  static void destroy(napi_deferred__ *deferred);

  ~napi_deferred__();

  JSValue call_resolve(napi_value resolution);
  JSValue call_reject(napi_value rejection);

private:
  napi_deferred__(napi_env env, JSValue resolve, JSValue reject);

  napi_env env_;
  JSValue resolve_;
  JSValue reject_;
};

#endif // NAPI_QUICKJS_DEFERRED_H_
