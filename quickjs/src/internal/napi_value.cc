#include "internal/napi_value.h"

#include "internal/napi_env.h"

#include <new>

napi_value__::napi_value__(napi_env env, JSValue value, bool owned)
    : env_(env),
      value_(owned ? value : JS_DupValue(env->context(), value))
{
}

napi_value__::~napi_value__()
{
  if (env_ != nullptr && env_->context() != nullptr)
  {
    JS_FreeValue(env_->context(), value_);
  }
}

napi_value__ *napi_value__::create(napi_env env, JSValue value, bool owned)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_value__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_value__(env, value, owned);
}

void napi_value__::destroy(napi_value__ *value)
{
  if (value == nullptr)
    return;

  napi_env env = value->env_;
  value->~napi_value__();
  if (env != nullptr && env->context() != nullptr)
  {
    js_free(env->context(), value);
  }
}

JSValueConst napi_value__::get_inner() const
{
  return value_;
}
