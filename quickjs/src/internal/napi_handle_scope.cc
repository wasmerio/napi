#include "internal/napi_handle_scope.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"

#include <new>

napi_handle_scope__::napi_handle_scope__(napi_env env, napi_scope__ *parent)
    : napi_scope__(env, parent)
{
  NAPI_QUICKJS_LIFETIME_RECORD(create, handle_scope, this, env);
}

napi_handle_scope__::~napi_handle_scope__()
{
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, handle_scope, this, env());
}

napi_handle_scope__ *napi_handle_scope__::create(napi_env env, napi_scope__ *parent)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_handle_scope__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_handle_scope__(env, parent);
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
