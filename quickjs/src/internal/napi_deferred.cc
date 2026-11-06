#include "internal/napi_deferred.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

napi_deferred__::napi_deferred__(napi_env env, JSValue resolve, JSValue reject)
    : env_{env},
      resolve_{resolve},
      reject_{reject},
      realm_ctx_{env->context()},
      realm_anchor_{JS_DupValue(env->context(), env->current_realm_anchor())}
{
}

napi_deferred__::~napi_deferred__()
{
  if (env_ == nullptr)
    return;

  JSContext *context = env_->main_context();
  JSValue resolve = resolve_;
  JSValue reject = reject_;
  JSValue anchor = realm_anchor_;
  env_ = nullptr;
  resolve_ = JS_UNDEFINED;
  reject_ = JS_UNDEFINED;
  realm_anchor_ = JS_UNDEFINED;
  realm_ctx_ = nullptr;

  if (context != nullptr)
  {
    JS_FreeValue(context, resolve);
    JS_FreeValue(context, reject);
    JS_FreeValue(context, anchor);
  }
}

napi_env napi_deferred__::env() const
{
  return env_;
}

napi_deferred__ *napi_deferred__::create(napi_env env, JSValue resolve, JSValue reject)
{
  if (env == nullptr)
    return nullptr;
  return env->create_deferred(resolve, reject);
}

void napi_deferred__::destroy(napi_deferred__ *deferred)
{
  if (deferred == nullptr)
    return;

  napi_env env = deferred->env_;
  if (env != nullptr)
    env->destroy_deferred(deferred);
}

// The promise settles in its own realm.
JSValue napi_deferred__::call(JSValueConst function, napi_value argument)
{
  napi_realm_frame_scope__ realm_frame{env_, realm_ctx_, realm_anchor_};
  JSValue arg = napi_quickjs_value_inner(env_, argument);
  return JS_Call(realm_ctx_, function, JS_UNDEFINED, 1, &arg);
}

JSValue napi_deferred__::call_resolve(napi_value resolution)
{
  return call(resolve_, resolution);
}

JSValue napi_deferred__::call_reject(napi_value rejection)
{
  return call(reject_, rejection);
}
