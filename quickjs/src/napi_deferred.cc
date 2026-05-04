#include "internal/napi_deferred.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

#include <new>

napi_deferred__::napi_deferred__(napi_env env, JSValue resolve, JSValue reject)
    : env_(env),
      resolve_(resolve),
      reject_(reject)
{
}

napi_deferred__::~napi_deferred__()
{
  if (env_ != nullptr && env_->context() != nullptr)
  {
    JS_FreeValue(env_->context(), resolve_);
    JS_FreeValue(env_->context(), reject_);
  }
}

napi_deferred__ *napi_deferred__::create(napi_env env, JSValue resolve, JSValue reject)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_deferred__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_deferred__(env, resolve, reject);
}

void napi_deferred__::destroy(napi_deferred__ *deferred)
{
  if (deferred == nullptr)
    return;

  napi_env env = deferred->env_;
  deferred->~napi_deferred__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), deferred);
}

JSValue napi_deferred__::call_resolve(napi_value resolution)
{
  JSValue arg = resolution->get_inner();
  return JS_Call(env_->context(), resolve_, JS_UNDEFINED, 1, &arg);
}

JSValue napi_deferred__::call_reject(napi_value rejection)
{
  JSValue arg = rejection->get_inner();
  return JS_Call(env_->context(), reject_, JS_UNDEFINED, 1, &arg);
}
