#include "internal/napi_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

#include <new>

napi_scope__::napi_scope__(napi_env env, napi_scope__ *parent)
    : env_(env),
      parent_(parent)
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
      ,
      values_(quickjs::detail::napi_lifetime_slot_kind::value),
      refs_(quickjs::detail::napi_lifetime_slot_kind::ref)
#endif
{
  if (parent_ != nullptr)
    values_.reserve_prefix(parent_->value_slot_count());
  NAPI_QUICKJS_LIFETIME_RECORD(create, scope, this, env_);
}

napi_scope__::~napi_scope__()
{
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, scope, this, env_);
  close();
}

napi_scope__ *napi_scope__::create(napi_env env, napi_scope__ *parent)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_scope__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_scope__(env, parent);
}

void napi_scope__::destroy(napi_scope__ *scope)
{
  if (scope == nullptr)
    return;

  napi_env env = scope->env_;
  scope->~napi_scope__();
  if (env != nullptr && env->context() != nullptr)
  {
    js_free(env->context(), scope);
  }
}

napi_value napi_scope__::wrap_value(JSValue value, bool owned)
{
  if (closed_)
  {
    if (owned && env_ != nullptr && env_->context() != nullptr)
      JS_FreeValue(env_->context(), value);
    return nullptr;
  }

  napi_value wrapped = values_.allocate(env_, value, owned);
  if (wrapped == nullptr)
  {
    if (owned && env_ != nullptr && env_->context() != nullptr)
      JS_FreeValue(env_->context(), value);
    return nullptr;
  }

  return wrapped;
}

napi_value napi_scope__::escape_value(napi_value value)
{
  if (parent_ == nullptr || value == nullptr)
    return nullptr;

  napi_value__ *slot = value_from_handle(value);
  if (slot == nullptr)
    return nullptr;
  return parent_->wrap_value(slot->get_inner(), false);
}

void napi_scope__::delete_value(napi_value value)
{
  if (value == nullptr)
    return;

  if (values_.get(value) != nullptr)
    values_.release(value);
  else if (parent_ != nullptr)
    parent_->delete_value(value);
}

napi_value__ *napi_scope__::value_from_handle(napi_value value)
{
  napi_value__ *slot = values_.get(value);
  if (slot != nullptr)
    return slot;
  return parent_ == nullptr ? nullptr : parent_->value_from_handle(value);
}

napi_ref napi_scope__::wrap_ref(JSValueConst value, uint32_t initial_ref_count)
{
  if (closed_ || env_ == nullptr || env_->context() == nullptr)
    return nullptr;
  return refs_.allocate(env_, value, initial_ref_count);
}

void napi_scope__::delete_ref(napi_ref ref)
{
  refs_.release(ref);
}

napi_ref__ *napi_scope__::ref_from_handle(napi_ref ref)
{
  return refs_.get(ref);
}

void napi_scope__::close()
{
  if (closed_)
    return;

  refs_.close();
  values_.close();
  closed_ = true;
}

size_t napi_scope__::value_slot_count() const
{
  return values_.slot_count();
}

napi_scope__ *napi_scope__::parent() const
{
  return parent_;
}

napi_env napi_scope__::env() const
{
  return env_;
}
