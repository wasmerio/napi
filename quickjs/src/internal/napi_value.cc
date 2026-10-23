#include "internal/napi_value.h"

#include "internal/napi_env.h"
#include "internal/napi_scope.h"

napi_value__::napi_value__(napi_env env, JSValue value, bool owned)
    : runtime_{env == nullptr ? nullptr : env->agent()->runtime()},
      value_{owned ? value : JS_DupValueRT(runtime_, value)}
{
}

napi_value__::~napi_value__()
{
  if (runtime_ == nullptr)
    return;

  JSRuntime *runtime = runtime_;
  JSValue value = value_;
  runtime_ = nullptr;
  value_ = JS_UNDEFINED;
  JS_FreeValueRT(runtime, value);
}

bool napi_value__::is_active() const
{
  return runtime_ != nullptr;
}

JSValueConst napi_value__::get_inner() const
{
  return value_;
}

JSValueConst napi_quickjs_value_inner(napi_env env, napi_value value)
{
  assert(env != nullptr);
  assert(value != nullptr);

  napi_value__ *data = env->value_from_handle(value);
  return data->get_inner();
}
