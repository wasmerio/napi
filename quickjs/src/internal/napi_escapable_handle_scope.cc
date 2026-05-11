#include "internal/napi_escapable_handle_scope.h"

#include "internal/napi_env.h"
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
#include "internal/napi_lifetime_tracker.h"
#endif

#include <new>

napi_escapable_handle_scope__::napi_escapable_handle_scope__(napi_env env, napi_scope__ *parent)
    : napi_scope__(env, parent)
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
  quickjs::detail::napi_lifetime_tracker__::record_create(
      quickjs::detail::napi_lifetime_kind::escapable_handle_scope, this, env);
#endif
}

napi_escapable_handle_scope__::~napi_escapable_handle_scope__()
{
#ifdef NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER
  quickjs::detail::napi_lifetime_tracker__::record_destroy(
      quickjs::detail::napi_lifetime_kind::escapable_handle_scope, this, env());
#endif
}

napi_escapable_handle_scope__ *napi_escapable_handle_scope__::create(napi_env env, napi_scope__ *parent)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_escapable_handle_scope__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_escapable_handle_scope__(env, parent);
}

void napi_escapable_handle_scope__::destroy(napi_escapable_handle_scope__ *scope)
{
  if (scope == nullptr)
    return;

  napi_env env = scope->env();
  scope->~napi_escapable_handle_scope__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), scope);
}

bool napi_escapable_handle_scope__::has_escaped() const
{
  return escaped_;
}

void napi_escapable_handle_scope__::mark_escaped()
{
  escaped_ = true;
}
