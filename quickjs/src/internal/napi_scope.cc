#include "internal/napi_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

napi_scope__::napi_scope__()
{
}

napi_scope__::~napi_scope__()
{
  release();
}

napi_scope__::napi_scope__(napi_scope__ &&other) noexcept
{
  *this = static_cast<napi_scope__ &&>(other);
}

napi_scope__ &napi_scope__::operator=(napi_scope__ &&other) noexcept
{
  if (this == &other)
    return *this;

  release();
  env_ = other.env_;
  level_ = other.level_;
  parent_ = other.parent_;
  values_ = static_cast<napi_allocator__<napi_value__> &&>(other.values_);
  refs_ = static_cast<napi_allocator__<napi_ref__> &&>(other.refs_);
  closed_ = other.closed_;
  active_ = other.active_;
  escaped_ = other.escaped_;
  other.env_ = nullptr;
  other.level_ = 0;
  other.parent_ = nullptr;
  other.closed_ = true;
  other.active_ = false;
  other.escaped_ = false;
  return *this;
}

void napi_scope__::initialize(napi_env env, napi_handle_scope parent)
{
  release();
  env_ = env;
  parent_ = parent;
  closed_ = false;
  active_ = true;
  escaped_ = false;

  napi_scope__ *parent_scope = this->parent();
  level_ = parent_scope == nullptr ? 0 : parent_scope->level() + 1;
  if (parent_scope != nullptr)
    values_.reserve_prefix(parent_scope->value_slot_count());
}

void napi_scope__::release()
{
  if (!active_)
    return;

  close();
  env_ = nullptr;
  level_ = 0;
  parent_ = nullptr;
  escaped_ = false;
  active_ = false;
}

bool napi_scope__::is_active() const
{
  return active_;
}

size_t napi_scope__::level() const
{
  return level_;
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

  NAPI_QUICKJS_LIFETIME_MAYBE_DUMP(env_);
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
  {
    values_.release(value);
    NAPI_QUICKJS_LIFETIME_MAYBE_DUMP(env_);
  }
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
  napi_ref wrapped = refs_.allocate(env_, value, initial_ref_count);
  NAPI_QUICKJS_LIFETIME_MAYBE_DUMP(env_);
  return wrapped;
}

void napi_scope__::delete_ref(napi_ref ref)
{
  refs_.release(ref);
  NAPI_QUICKJS_LIFETIME_MAYBE_DUMP(env_);
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
  NAPI_QUICKJS_LIFETIME_MAYBE_DUMP(env_);
}

size_t napi_scope__::value_slot_count() const
{
  return values_.slot_count();
}

size_t napi_scope__::value_storage_slot_count() const
{
  return values_.storage_slot_count();
}

size_t napi_scope__::active_value_count() const
{
  return values_.active_count();
}

size_t napi_scope__::ref_storage_slot_count() const
{
  return refs_.storage_slot_count();
}

size_t napi_scope__::active_ref_count() const
{
  return refs_.active_count();
}

napi_handle_scope napi_scope__::parent_handle() const
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

bool napi_scope__::has_escaped() const
{
  return escaped_;
}

void napi_scope__::mark_escaped()
{
  escaped_ = true;
}
