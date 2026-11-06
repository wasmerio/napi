#include "internal/napi_external_backing_store_hint.h"

#include "internal/napi_env.h"

napi_external_backing_store_hint__::napi_external_backing_store_hint__(
    napi_env env,
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
    : env_{env},
      rt_{env == nullptr || env->context() == nullptr ? nullptr : JS_GetRuntime(env->context())},
      external_data_{external_data},
      finalize_cb_{finalize_cb},
      finalize_hint_{finalize_hint}
{
}

napi_external_backing_store_hint__::~napi_external_backing_store_hint__()
{
  if (pending_link_.linked())
    pending_link_.unlink();
  forget_array_buffer();
  env_ = nullptr;
  rt_ = nullptr;
  external_data_ = nullptr;
  finalize_cb_ = nullptr;
  finalize_hint_ = nullptr;
  finalize_invoked_ = false;
  released_ = false;
  weak_target_ = JS_UNDEFINED;
}

napi_external_backing_store_hint__ *napi_external_backing_store_hint__::create(
    napi_env env,
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
{
  if (env == nullptr || env->context() == nullptr)
    return nullptr;
  return env->create_external_backing_store(external_data, finalize_cb, finalize_hint);
}

void napi_external_backing_store_hint__::destroy(napi_external_backing_store_hint__ *hint)
{
  if (hint == nullptr)
    return;

  destroy_with_runtime(hint->rt_, hint);
}

void napi_external_backing_store_hint__::destroy_with_runtime(
    JSRuntime *rt,
    napi_external_backing_store_hint__ *hint)
{
  if (hint == nullptr)
    return;

  (void)rt;
  napi_env env = hint->env_;
  if (env != nullptr)
    env->destroy_external_backing_store(hint);
}

void napi_external_backing_store_hint__::invoke_finalizer()
{
  if (finalize_invoked_)
    return;
  finalize_invoked_ = true;
  if (pending_link_.linked())
    pending_link_.unlink();
  forget_array_buffer();
  if (finalize_cb_ == nullptr)
    return;
  if (env_ == nullptr)
  {
    finalize_cb_(env_, external_data_, finalize_hint_);
    return;
  }
  // Teardown has run every finalizer it could, and the context may be gone
  // (the runtime frees what is still alive): no user code runs now.
  if (env_->finalizers_done())
    return;
  // Finalizers run in the env's own realm, also when the collector runs
  // inside another realm.
  napi_realm_frame_scope__ realm_frame{env_, nullptr, JS_UNDEFINED};
  finalize_cb_(env_, external_data_, finalize_hint_);
}

void napi_external_backing_store_hint__::release_array_buffer(JSRuntime *rt)
{
  // QuickJS marks the buffer detached only once this returns, so a finalizer
  // that detaches the same buffer again calls back in here: the outer call
  // finishes, and destroys the hint once.
  if (released_)
    return;
  released_ = true;
  invoke_finalizer();
  if (!detaching_)
    destroy_with_runtime(rt, this);
}

void napi_external_backing_store_hint__::track_array_buffer(JSContext *ctx,
                                                            JSValueConst array_buffer)
{
  // Only a finalizer frees the memory, so only then must teardown detach the
  // buffer first.
  if (finalize_cb_ == nullptr || finalize_invoked_)
    return;
  if (JS_AddNativeWeakRefLink(ctx, array_buffer, &array_buffer_link_, array_buffer_freed, this) < 0)
  {
    // Out of memory: teardown can still run the finalizer, just not detach.
    if (JS_HasException(ctx))
      JS_FreeValue(ctx, JS_GetException(ctx));
    return;
  }
  array_buffer_ = array_buffer;
}

void napi_external_backing_store_hint__::link_pending(napi_intrinsic_link__ &pending)
{
  pending.link(pending_link_);
}

napi_external_backing_store_hint__ *napi_external_backing_store_hint__::first_pending(
    napi_intrinsic_link__ &pending)
{
  napi_intrinsic_link__ *link = pending.first();
  return link == nullptr
             ? nullptr
             : link->unsafe_get<&napi_external_backing_store_hint__::pending_link_>();
}

void napi_external_backing_store_hint__::finalize_at_teardown()
{
  if (JS_GetNativeWeakRefFromLink(&array_buffer_link_) != nullptr)
  {
    // As Node does, detach the buffer first, so nothing can reach the memory
    // the finalizer frees. Detaching runs the finalizer, unless
    // ArrayBuffer.prototype.transfer already moved the memory to a buffer this
    // hint cannot reach (Node cannot either). The reference keeps the buffer
    // alive in case the finalizer drops the last other one.
    JSContext *ctx = env_->main_context();
    JSValue array_buffer = JS_DupValue(ctx, array_buffer_);
    detaching_ = true;
    JS_DetachArrayBuffer(ctx, array_buffer);
    detaching_ = false;
    JS_FreeValue(ctx, array_buffer);
    if (finalize_invoked_)
    {
      // The buffer no longer refers to the hint.
      destroy(this);
      return;
    }
  }
  invoke_finalizer();
}

void napi_external_backing_store_hint__::abandon_finalizer()
{
  if (pending_link_.linked())
    pending_link_.unlink();
  forget_array_buffer();
}

void napi_external_backing_store_hint__::array_buffer_freed(JSRuntime *rt, void *opaque)
{
  (void)rt;
  // QuickJS has already cleared the link.
  static_cast<napi_external_backing_store_hint__ *>(opaque)->array_buffer_ = JS_UNDEFINED;
}

void napi_external_backing_store_hint__::forget_array_buffer()
{
  if (JS_GetNativeWeakRefFromLink(&array_buffer_link_) != nullptr)
    JS_DeleteNativeWeakRefLink(rt_, &array_buffer_link_);
  array_buffer_ = JS_UNDEFINED;
}

napi_env napi_external_backing_store_hint__::env() const
{
  return env_;
}

JSRuntime *napi_external_backing_store_hint__::runtime() const
{
  return rt_;
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
