#include "internal/napi_ref.h"

#include "internal/napi_env.h"

namespace
{
bool IsEmptyValue(JSValueConst value)
{
  return JS_IsUninitialized(value) || JS_IsUndefined(value) || JS_IsNull(value);
}

bool SameRefCountedValue(JSValueConst left, JSValueConst right)
{
  return JS_VALUE_HAS_REF_COUNT(left) &&
         JS_VALUE_HAS_REF_COUNT(right) &&
         JS_VALUE_GET_TAG(left) == JS_VALUE_GET_TAG(right) &&
         JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
}
} // namespace

napi_ref__::napi_ref__(napi_ref__ &&other) noexcept
    : env_(other.env_),
      value_(other.value_),
      can_be_weak_(other.can_be_weak_),
      ref_count_(other.ref_count_)
{
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
  other.can_be_weak_ = false;
  other.ref_count_ = 0;
}

napi_ref__ &napi_ref__::operator=(napi_ref__ &&other) noexcept
{
  if (this == &other)
    return *this;

  release();
  env_ = other.env_;
  value_ = other.value_;
  can_be_weak_ = other.can_be_weak_;
  ref_count_ = other.ref_count_;
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
  other.can_be_weak_ = false;
  other.ref_count_ = 0;
  return *this;
}

napi_ref__::~napi_ref__()
{
  release();
}

void napi_ref__::initialize(napi_env env,
                            JSValueConst value,
                            uint32_t initial_ref_count)
{
  release();
  env_ = env;
  value_ = value;
  can_be_weak_ = JS_VALUE_HAS_REF_COUNT(value);
  ref_count_ = initial_ref_count;
  if (ref_count_ > 0)
    JS_DupValue(env_->context(), value_);
}

void napi_ref__::release()
{
  if (env_ == nullptr)
    return;

  napi_env env = env_;
  JSValue value = value_;
  uint32_t ref_count = ref_count_;
  env_ = nullptr;
  value_ = JS_UNDEFINED;
  can_be_weak_ = false;
  ref_count_ = 0;

  if (env != nullptr && env->context() != nullptr && ref_count > 0)
    JS_FreeValue(env->context(), value);
}

bool napi_ref__::is_active() const
{
  return env_ != nullptr;
}

uint32_t napi_ref__::add_ref()
{
  if (is_empty())
    return ref_count_;

  if (ref_count_ == 0)
    JS_DupValue(env_->context(), value_);
  ++ref_count_;
  return ref_count_;
}

uint32_t napi_ref__::rem_ref()
{
  if (ref_count_ == 0)
    return ref_count_;

  --ref_count_;
  uint32_t ref_count = ref_count_;
  if (ref_count == 0)
  {
    napi_env env = env_;
    JSValue value = value_;
    bool can_be_weak = can_be_weak_;
    if (!can_be_weak)
    {
      value_ = JS_UNDEFINED;
    }
    if (env != nullptr && env->context() != nullptr)
      JS_FreeValue(env->context(), value);
  }
  return ref_count;
}

uint32_t napi_ref__::ref_count() const
{
  return ref_count_;
}

bool napi_ref__::can_be_weak() const
{
  return can_be_weak_;
}

bool napi_ref__::is_empty() const
{
  return IsEmptyValue(value_);
}

bool napi_ref__::is_weak() const
{
  return can_be_weak_ && ref_count_ == 0 && !is_empty();
}

napi_env napi_ref__::env() const
{
  return env_;
}

JSValueConst napi_ref__::get_inner() const
{
  return value_;
}

JSValue napi_ref__::dup_inner() const
{
  return JS_DupValue(env_->context(), value_);
}

void napi_ref__::clear_if_matches(JSValueConst value)
{
  if (is_weak() && SameRefCountedValue(value_, value))
  {
    value_ = JS_UNDEFINED;
  }
}

napi_ref__ *napi_quickjs_ref_slot(napi_env env, napi_ref ref)
{
  if (env == nullptr || ref == nullptr)
    return nullptr;
  return env->ref_from_root_scope(ref);
}
