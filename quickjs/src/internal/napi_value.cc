#include "internal/napi_value.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"
#include "internal/napi_scope.h"

napi_value__::napi_value__(napi_value__ &&other) noexcept
    : env_(other.env_),
      value_(other.value_),
      active_(other.active_)
{
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
  other.active_ = false;
}

napi_value__ &napi_value__::operator=(napi_value__ &&other) noexcept
{
  if (this == &other)
    return *this;

  release();
  env_ = other.env_;
  value_ = other.value_;
  active_ = other.active_;
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
  other.active_ = false;
  return *this;
}

napi_value__::~napi_value__()
{
  release();
}

void napi_value__::initialize(napi_env env, JSValue value, bool owned)
{
  release();
  env_ = env;
  value_ = owned ? value : JS_DupValue(env->context(), value);
  active_ = true;
  NAPI_QUICKJS_LIFETIME_RECORD(create, value, this, env_);
}

void napi_value__::release()
{
  if (!active_)
    return;

  NAPI_QUICKJS_LIFETIME_RECORD(destroy, value, this, env_);
  if (env_ != nullptr && env_->context() != nullptr)
  {
    JS_FreeValue(env_->context(), value_);
  }
  env_ = nullptr;
  value_ = JS_UNDEFINED;
  active_ = false;
}

bool napi_value__::is_active() const
{
  return active_;
}

JSValueConst napi_value__::get_inner() const
{
  return value_;
}

napi_value__ *napi_quickjs_value_slot(napi_env env, napi_value value)
{
  if (env == nullptr || value == nullptr || env->current_scope() == nullptr)
    return nullptr;
  return env->current_scope()->value_from_handle(value);
}

JSValueConst napi_quickjs_value_inner(napi_env env, napi_value value)
{
  napi_value__ *slot = napi_quickjs_value_slot(env, value);
  return slot == nullptr ? JS_UNDEFINED : slot->get_inner();
}
