#include "internal/napi_env_cleanup_hook.h"

#include "internal/napi_env.h"
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
#include "internal/napi_lifetime_tracker.h"
#endif

#include <new>

napi_env_cleanup_hook__::napi_env_cleanup_hook__(napi_env env, napi_cleanup_hook hook, void *arg)
    : env_(env),
      hook_(hook),
      arg_(arg)
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
  quickjs::detail::napi_lifetime_tracker__::record_create(
      quickjs::detail::napi_lifetime_kind::cleanup_hook, this, env_);
#endif
}

napi_env_cleanup_hook__::~napi_env_cleanup_hook__()
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
  quickjs::detail::napi_lifetime_tracker__::record_destroy(
      quickjs::detail::napi_lifetime_kind::cleanup_hook, this, env_);
#endif
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
