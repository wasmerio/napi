#include "internal/napi_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_tracker.h"

napi_scope__::napi_scope__() : values_(this)
{
}

napi_scope__::~napi_scope__()
{
  release();
}

void napi_scope__::initialize(napi_env env, napi_handle_scope parent)
{
  release();
  env_ = env;
  values_.set_owner(this);
  parent_ = parent;
  closed_ = false;
  escaped_ = false;

  napi_scope__ *parent_scope = this->parent();
  level_ = parent_scope == nullptr ? 0 : parent_scope->level() + 1;
  if (parent_scope != nullptr)
    values_.reserve_prefix(parent_scope->value_slot_count());
}

void napi_scope__::release()
{
  if (env_ == nullptr)
    return;

  close();
  env_ = nullptr;
  level_ = 0;
  parent_ = nullptr;
  escaped_ = false;
}

bool napi_scope__::is_active() const
{
  return env_ != nullptr;
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

  return wrapped;
}

napi_value napi_scope__::escape_value(napi_value value)
{
  auto record_escape = [this](napi_value escaped) {
    quickjs::detail::napi_lifetime_tracker__::record_scope_escape(env_, escaped != nullptr);
    return escaped;
  };

  if (parent_ == nullptr || value == nullptr)
    return record_escape(nullptr);

  auto [slot, owner] = values_.unsafe_data_with_owner_from_handle(value);
  assert(owner == this);

  if (owner != this)
    return record_escape(nullptr);

  napi_scope__ *parent_scope = parent();
  return record_escape(parent_scope == nullptr ? nullptr : parent_scope->wrap_value(slot->get_inner(), false));
}

void napi_scope__::delete_value(napi_value value)
{
  if (value == nullptr)
    return;

  auto [slot, owner] = values_.unsafe_data_with_owner_from_handle(value);
  (void)slot;

  if (owner == this)
  {
    values_.release(value);
  }
}

void napi_scope__::close()
{
  if (closed_)
    return;

  values_.close();
  closed_ = true;
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

napi_handle_scope napi_scope__::parent_handle() const
{
  return parent_;
}

napi_scope__ *napi_scope__::parent() const
{
  return env_ == nullptr || parent_ == nullptr ? nullptr : env_->scope_from_handle(parent_);
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
