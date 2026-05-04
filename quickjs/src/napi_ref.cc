#include "internal/napi_ref.h"

#include "internal/napi_env.h"

#include <new>

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

napi_ref__::napi_ref__(napi_env env, JSValueConst value, uint32_t initial_ref_count)
    : env_(env),
      value_(value),
      can_be_weak_(JS_VALUE_HAS_REF_COUNT(value)),
      ref_count_(initial_ref_count)
{
  if (ref_count_ > 0)
    JS_DupValue(env_->context(), value_);
}

napi_ref__::~napi_ref__()
{
  if (env_ != nullptr && env_->context() != nullptr && ref_count_ > 0)
    JS_FreeValue(env_->context(), value_);
}

napi_ref__ *napi_ref__::create(napi_env env, JSValueConst value, uint32_t initial_ref_count)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_ref__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_ref__(env, value, initial_ref_count);
}

void napi_ref__::destroy(napi_ref__ *ref)
{
  if (ref == nullptr)
    return;

  napi_env env = ref->env_;
  ref->~napi_ref__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), ref);
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
  if (ref_count_ == 0)
  {
    JS_FreeValue(env_->context(), value_);
    if (!can_be_weak_)
      value_ = JS_UNDEFINED;
  }
  return ref_count_;
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
    value_ = JS_UNDEFINED;
}
