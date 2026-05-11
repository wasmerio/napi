#include "internal/napi_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

napi_scope__::napi_scope__()
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
    : values_(quickjs::detail::napi_lifetime_slot_kind::value),
      refs_(quickjs::detail::napi_lifetime_slot_kind::ref)
#endif
{
}

napi_scope__::~napi_scope__()
{
  release();
}

napi_scope__::napi_scope__(napi_scope__ &&other) noexcept
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
    : values_(quickjs::detail::napi_lifetime_slot_kind::value),
      refs_(quickjs::detail::napi_lifetime_slot_kind::ref)
#endif
{
  *this = static_cast<napi_scope__ &&>(other);
}

napi_scope__ &napi_scope__::operator=(napi_scope__ &&other) noexcept
{
  if (this == &other)
    return *this;

  release();
  env_ = other.env_;
  index_ = other.index_;
  parent_ = other.parent_;
  values_ = static_cast<napi_allocator__<napi_value__> &&>(other.values_);
  refs_ = static_cast<napi_allocator__<napi_ref__> &&>(other.refs_);
  closed_ = other.closed_;
  active_ = other.active_;
  other.env_ = nullptr;
  other.index_ = 0;
  other.parent_ = nullptr;
  other.closed_ = true;
  other.active_ = false;
  return *this;
}

void napi_scope__::initialize(napi_env env, napi_scope_handle__ parent)
{
  release();
  env_ = env;
  index_ = 0;
  parent_ = parent;
  closed_ = false;
  active_ = true;

  napi_scope__ *parent_scope = this->parent();
  if (parent_scope != nullptr)
    values_.reserve_prefix(parent_scope->value_slot_count());
  NAPI_QUICKJS_LIFETIME_RECORD(create, scope, this, env_);
}

void napi_scope__::release()
{
  if (!active_)
    return;

  close();
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, scope, this, env_);
  env_ = nullptr;
  index_ = 0;
  parent_ = nullptr;
  active_ = false;
}

bool napi_scope__::is_active() const
{
  return active_;
}

void napi_scope__::set_index(size_t index)
{
  index_ = index;
}

size_t napi_scope__::index() const
{
  return index_;
}

napi_value napi_scope__::wrap_value(JSValue value, bool owned)
{
  if (closed_)
  {
    if (owned && env_ != nullptr && env_->context() != nullptr)
      JS_FreeValue(env_->context(), value);
    return nullptr;
  }

  napi_value wrapped = values_.allocate(env_, index_, value, owned);
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
  napi_scope__ *parent_scope = parent();
  return parent_scope == nullptr ? nullptr : parent_scope->wrap_value(slot->get_inner(), false);
}

void napi_scope__::delete_value(napi_value value)
{
  if (value == nullptr)
    return;

  if (values_.get(value) != nullptr)
    values_.release(value);
  else if (parent() != nullptr)
    parent()->delete_value(value);
}

napi_value__ *napi_scope__::value_from_handle(napi_value value)
{
  napi_value__ *slot = values_.get(value);
  if (slot != nullptr)
    return slot;
  napi_scope__ *parent_scope = parent();
  return parent_scope == nullptr ? nullptr : parent_scope->value_from_handle(value);
}

napi_ref napi_scope__::wrap_ref(JSValueConst value, uint32_t initial_ref_count)
{
  if (closed_ || env_ == nullptr || env_->context() == nullptr)
    return nullptr;
  return refs_.allocate(env_, index_, value, initial_ref_count);
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

napi_scope_handle__ napi_scope__::parent_handle() const
{
  return parent_;
}

napi_scope__ *napi_scope__::parent() const
{
  return env_ == nullptr ? nullptr : env_->scope_from_handle(parent_);
}

napi_env napi_scope__::env() const
{
  return env_;
}
