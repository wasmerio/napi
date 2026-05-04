#include "internal/napi_external_backing_store_hint.h"

#include "internal/napi_env.h"

#include <new>

napi_external_backing_store_hint__::napi_external_backing_store_hint__(
    napi_env env,
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
    : env_(env),
      external_data_(external_data),
      finalize_cb_(finalize_cb),
      finalize_hint_(finalize_hint)
{
}

napi_external_backing_store_hint__ *napi_external_backing_store_hint__::create(
    napi_env env,
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;

  void *memory = js_mallocz(env->context(), sizeof(napi_external_backing_store_hint__));
  if (memory == nullptr)
    return nullptr;

  return new (memory) napi_external_backing_store_hint__(env, external_data, finalize_cb, finalize_hint);
}

void napi_external_backing_store_hint__::destroy(napi_external_backing_store_hint__ *hint)
{
  if (hint == nullptr)
    return;

  napi_env env = hint->env_;
  hint->~napi_external_backing_store_hint__();
  if (env != nullptr && env->context() != nullptr)
    js_free(env->context(), hint);
}

void napi_external_backing_store_hint__::invoke_finalizer() const
{
  if (finalize_cb_ != nullptr)
    finalize_cb_(env_, external_data_, finalize_hint_);
}

napi_env napi_external_backing_store_hint__::env() const
{
  return env_;
}

void *napi_external_backing_store_hint__::external_data() const
{
  return external_data_;
}

JSValue napi_external_backing_store_hint__::weak_target() const
{
  return weak_target_;
}

JSValue napi_external_backing_store_hint__::finalizer_target(JSValue fallback) const
{
  return JS_IsUndefined(weak_target_) ? fallback : weak_target_;
}

void napi_external_backing_store_hint__::set_weak_target(JSValue weak_target)
{
  weak_target_ = weak_target;
}
