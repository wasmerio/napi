#include "internal/napi_value.h"

#include "internal/quickjs_env.h"

#include <new>

napi_value__::napi_value__(napi_env env, JSValue value, bool owned)
    : env_(env),
      value_(owned ? value : JS_DupValue(env->ctx, value))
{
}

napi_value__::~napi_value__()
{
  if (env_ != nullptr && env_->ctx != nullptr)
  {
    JS_FreeValue(env_->ctx, value_);
  }
}

napi_value__ *napi_value__::create(napi_env env, JSValue value, bool owned)
{
  if (env == nullptr || env->ctx == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->ctx, sizeof(napi_value__));
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
  if (env != nullptr && env->ctx != nullptr)
  {
    js_free(env->ctx, value);
  }
}

JSValueConst napi_value__::get_inner() const
{
  return value_;
}

napi_scope__::napi_scope__(napi_env env, napi_scope__ *parent)
    : env_(env),
      parent_(parent)
{
}

napi_scope__::~napi_scope__()
{
  close();
}

napi_scope__ *napi_scope__::create(napi_env env, napi_scope__ *parent)
{
  if (env == nullptr || env->ctx == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->ctx, sizeof(napi_scope__));
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
  if (env != nullptr && env->ctx != nullptr)
  {
    js_free(env->ctx, scope);
  }
}

napi_value napi_scope__::wrap_value(JSValue value, bool owned)
{
  if (closed_)
  {
    if (owned && env_ != nullptr && env_->ctx != nullptr)
      JS_FreeValue(env_->ctx, value);
    return nullptr;
  }

  napi_value wrapped = napi_value__::create(env_, value, owned);
  if (wrapped == nullptr)
  {
    if (owned && env_ != nullptr && env_->ctx != nullptr)
      JS_FreeValue(env_->ctx, value);
    return nullptr;
  }

  values_.push_back(wrapped);
  return wrapped;
}

napi_value napi_scope__::escape_value(napi_value value)
{
  if (parent_ == nullptr || value == nullptr)
    return nullptr;

  return parent_->wrap_value(value->get_inner(), false);
}

void napi_scope__::delete_value(napi_value value)
{
  if (value == nullptr)
    return;

  for (auto it = values_.begin(); it != values_.end(); ++it)
  {
    if (*it == value)
    {
      values_.erase(it);
      napi_value__::destroy(value);
      break;
    }
  }
}

void napi_scope__::close()
{
  if (closed_)
    return;

  for (auto it = values_.rbegin(); it != values_.rend(); ++it)
  {
    napi_value__::destroy(*it);
  }
  values_.clear();
  closed_ = true;
}

napi_scope__ *napi_scope__::parent() const
{
  return parent_;
}
