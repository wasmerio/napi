#include "internal/napi_env_cleanup_hook.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

#include <new>

napi_env_cleanup_hook__::napi_env_cleanup_hook__(napi_env env, napi_cleanup_hook hook, void *arg)
    : env_(env),
      hook_(hook),
      arg_(arg)
{
  NAPI_QUICKJS_LIFETIME_RECORD(create, cleanup_hook, this, env_);
}

napi_env_cleanup_hook__::~napi_env_cleanup_hook__()
{
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, cleanup_hook, this, env_);
}

napi_env_cleanup_hook__ *napi_env_cleanup_hook__::create(napi_env env, napi_cleanup_hook hook, void *arg)
{
  if (env == nullptr || env->context() == nullptr || hook == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_env_cleanup_hook__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_env_cleanup_hook__(env, hook, arg);
}

void napi_env_cleanup_hook__::destroy(napi_env_cleanup_hook__ *entry)
{
  if (entry == nullptr)
    return;

  napi_env env = entry->env_;
  entry->~napi_env_cleanup_hook__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), entry);
}

void napi_env_cleanup_hook__::run() const
{
  if (hook_ != nullptr)
    hook_(arg_);
}

bool napi_env_cleanup_hook__::matches(napi_cleanup_hook hook, void *arg) const
{
  return hook_ == hook && arg_ == arg;
}
