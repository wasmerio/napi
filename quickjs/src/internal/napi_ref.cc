#include "internal/napi_ref.h"

#include "internal/napi_env.h"

namespace
{
bool IsEmptyValue(JSValueConst value)
{
  return JS_IsUninitialized(value) || JS_IsUndefined(value) || JS_IsNull(value);
}

} // namespace

napi_ref__::napi_ref__(napi_ref__ &&other) noexcept
    : env_(other.env_),
      value_(other.value_),
      ref_count_(other.ref_count_)
{
  if (JS_GetNativeWeakRefFromLink(&other.weak_link_) != nullptr)
  {
    JS_AddNativeWeakRefLink(
        other.env_->context(), other.value_, &weak_link_, weak_target_finalized, this);
    JS_DeleteNativeWeakRefLink(JS_GetRuntime(other.env_->context()), &other.weak_link_);
  }
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
  other.ref_count_ = 0;
}

napi_ref__ &napi_ref__::operator=(napi_ref__ &&other) noexcept
{
  if (this == &other)
    return *this;

  release();
  env_ = other.env_;
  value_ = other.value_;
  ref_count_ = other.ref_count_;
  if (JS_GetNativeWeakRefFromLink(&other.weak_link_) != nullptr)
  {
    JS_AddNativeWeakRefLink(
        other.env_->context(), other.value_, &weak_link_, weak_target_finalized, this);
    JS_DeleteNativeWeakRefLink(JS_GetRuntime(other.env_->context()), &other.weak_link_);
  }
  other.env_ = nullptr;
  other.value_ = JS_UNDEFINED;
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
  ref_count_ = initial_ref_count;
  if (ref_count_ > 0)
    JS_DupValue(env_->context(), value_);
  else if (!make_weak() && JS_VALUE_HAS_REF_COUNT(value_))
    JS_DupValue(env_->context(), value_);
}

void napi_ref__::release()
{
  if (env_ == nullptr)
    return;

  napi_env env = env_;
  JSValue value = value_;
  const bool owns_strong_value =
      JS_GetNativeWeakRefFromLink(&weak_link_) == nullptr && !IsEmptyValue(value) &&
      JS_VALUE_HAS_REF_COUNT(value);
  delete_weak_handle();
  env_ = nullptr;
  value_ = JS_UNDEFINED;
  ref_count_ = 0;

  if (env != nullptr && env->context() != nullptr && owns_strong_value)
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
  {
    const bool was_weak = JS_GetNativeWeakRefFromLink(&weak_link_) != nullptr;
    delete_weak_handle();
    if (is_empty())
      return ref_count_;
    if (was_weak)
      JS_DupValue(env_->context(), value_);
  }
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
    const bool became_weak = make_weak();
    if (became_weak && env != nullptr && env->context() != nullptr)
      JS_FreeValue(env->context(), value);
  }
  return ref_count;
}

uint32_t napi_ref__::ref_count() const
{
  return ref_count_;
}

bool napi_ref__::is_empty() const
{
  return IsEmptyValue(value_);
}

bool napi_ref__::is_weak() const
{
  return JS_GetNativeWeakRefFromLink(const_cast<JSNativeWeakRefLink *>(&weak_link_)) != nullptr;
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

void napi_ref__::weak_target_finalized(JSRuntime *rt, void *opaque)
{
  (void)rt;
  auto *ref = static_cast<napi_ref__ *>(opaque);
  if (ref != nullptr)
    ref->clear_weak_target();
}

void napi_ref__::clear_weak_target()
{
  value_ = JS_UNDEFINED;
  ref_count_ = 0;
}

void napi_ref__::delete_weak_handle()
{
  if (JS_GetNativeWeakRefFromLink(&weak_link_) == nullptr || env_ == nullptr ||
      env_->context() == nullptr)
    return;
  JS_DeleteNativeWeakRefLink(JS_GetRuntime(env_->context()), &weak_link_);
}

bool napi_ref__::make_weak()
{
  delete_weak_handle();
  if (env_ == nullptr || env_->context() == nullptr || is_empty())
    return false;
  const int rc =
      JS_AddNativeWeakRefLink(env_->context(), value_, &weak_link_, weak_target_finalized, this);
  if (rc < 0 && JS_HasException(env_->context()))
  {
    JSValue exception = JS_GetException(env_->context());
    JS_FreeValue(env_->context(), exception);
  }
  return JS_GetNativeWeakRefFromLink(&weak_link_) != nullptr;
}

napi_ref__ *napi_quickjs_ref_slot(napi_env env, napi_ref ref)
{
  if (env == nullptr || ref == nullptr)
    return nullptr;
  return env->ref_from_root_scope(ref);
}
