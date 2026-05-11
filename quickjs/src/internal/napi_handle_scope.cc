#include "internal/napi_handle_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

#include <new>

napi_handle_scope__::napi_handle_scope__(napi_env env)
    : env_(env)
{
  NAPI_QUICKJS_LIFETIME_RECORD(create, handle_scope, this, env);
}

napi_handle_scope__::~napi_handle_scope__()
{
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, handle_scope, this, env());
  if (env_ != nullptr && scope_ != nullptr)
    env_->destroy_scope(scope_);
  scope_ = nullptr;
  env_ = nullptr;
}

napi_handle_scope__ *napi_handle_scope__::create(napi_env env, napi_scope_handle__ parent)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_handle_scope__));
  if (memory == nullptr)
    return nullptr;

  auto *scope = new (memory) napi_handle_scope__(env);
  scope->scope_ = env->create_scope(parent);
  if (scope->scope_ == nullptr)
  {
    scope->~napi_handle_scope__();
    js_free(env->context(), scope);
    return nullptr;
  }
  return scope;
}

void napi_handle_scope__::destroy(napi_handle_scope__ *scope)
{
  if (scope == nullptr)
    return;

  napi_env env = scope->env();
  scope->~napi_handle_scope__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), scope);
}

napi_scope_handle__ napi_handle_scope__::scope_handle() const
{
  return scope_;
}

napi_scope_handle__ napi_handle_scope__::parent_handle() const
{
  napi_scope__ *inner = scope();
  return inner == nullptr ? nullptr : inner->parent_handle();
}

napi_scope__ *napi_handle_scope__::scope() const
{
  return env_ == nullptr ? nullptr : env_->scope_from_handle(scope_);
}

napi_scope__ *napi_handle_scope__::parent() const
{
  return env_ == nullptr ? nullptr : env_->scope_from_handle(parent_handle());
}

napi_env napi_handle_scope__::env() const
{
  return env_;
}
