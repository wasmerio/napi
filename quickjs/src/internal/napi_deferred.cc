#include "internal/napi_deferred.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

napi_deferred__::napi_deferred__(napi_env env, JSValue resolve, JSValue reject)
    : env_{env},
      resolve_{resolve},
      reject_{reject}
{
}

napi_deferred__::~napi_deferred__()
{
  if (env_ == nullptr)
    return;

  napi_env env = env_;
  JSValue resolve = resolve_;
  JSValue reject = reject_;
  env_ = nullptr;
  resolve_ = JS_UNDEFINED;
  reject_ = JS_UNDEFINED;

  if (env != nullptr && env->context() != nullptr)
  {
    JS_FreeValue(env->context(), resolve);
    JS_FreeValue(env->context(), reject);
  }
}

napi_env napi_deferred__::env() const
{
  return env_;
}

napi_deferred__ *napi_deferred__::create(napi_env env, JSValue resolve, JSValue reject)
{
  if (env == nullptr || env->context() == nullptr)
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

JSValue napi_deferred__::call_resolve(napi_value resolution)
{
  JSValue arg = napi_quickjs_value_inner(env_, resolution);
  return JS_Call(env_->context(), resolve_, JS_UNDEFINED, 1, &arg);
}

JSValue napi_deferred__::call_reject(napi_value rejection)
{
  JSValue arg = napi_quickjs_value_inner(env_, rejection);
  return JS_Call(env_->context(), reject_, JS_UNDEFINED, 1, &arg);
}
