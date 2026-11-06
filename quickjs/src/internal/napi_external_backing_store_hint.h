#ifndef NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_
#define NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"
#include "napi_intrinsic_link.h"

#include <quickjs.h>

typedef struct napi_external_backing_store_hint__ *napi_external_backing_store_hint;

struct napi_external_backing_store_hint__
{
  napi_external_backing_store_hint__(napi_env env,
                                     void *external_data,
                                     node_api_basic_finalize finalize_cb,
                                     void *finalize_hint);
  static napi_external_backing_store_hint__ *create(
      napi_env env,
      void *external_data,
      node_api_basic_finalize finalize_cb,
      void *finalize_hint);
  static void destroy(napi_external_backing_store_hint__ *hint);
  static void destroy_with_runtime(JSRuntime *rt, napi_external_backing_store_hint__ *hint);
  ~napi_external_backing_store_hint__();
  napi_external_backing_store_hint__(const napi_external_backing_store_hint__ &) = delete;
  napi_external_backing_store_hint__(napi_external_backing_store_hint__ &&other) = delete;
  napi_external_backing_store_hint__ &operator=(const napi_external_backing_store_hint__ &) = delete;
  napi_external_backing_store_hint__ &operator=(napi_external_backing_store_hint__ &&other) = delete;

  void invoke_finalizer();
  // The external array buffer this hint backs let go of the memory: it was
  // detached or freed.
  void release_array_buffer(JSRuntime *rt);
  // Remembers the external array buffer this hint backs, so teardown can
  // detach it before running the finalizer.
  void track_array_buffer(JSContext *ctx, JSValueConst array_buffer);

  // Env teardown. A hint whose finalizer has yet to run is on its env's
  // pending list until it runs or the hint is destroyed.
  void link_pending(napi_intrinsic_link__ &pending);
  static napi_external_backing_store_hint__ *first_pending(napi_intrinsic_link__ &pending);
  // Runs the finalizer, detaching the array buffer first. May destroy the
  // hint.
  void finalize_at_teardown();
  // Leaves the pending list for good: the finalizer will not run.
  void abandon_finalizer();

  napi_env env() const;
  JSRuntime *runtime() const;
  void *external_data() const;
  JSValue weak_target() const;
  JSValue finalizer_target(JSValue fallback) const;
  void set_weak_target(JSValue weak_target);

private:
  static void array_buffer_freed(JSRuntime *rt, void *opaque);
  void forget_array_buffer();

  // Owning runtime state.
  napi_env env_ = nullptr;
  JSRuntime *rt_ = nullptr;

  // Embedder backing store and finalizer.
  void *external_data_ = nullptr;
  node_api_basic_finalize finalize_cb_ = nullptr;
  void *finalize_hint_ = nullptr;
  bool finalize_invoked_ = false;

  // Optional JavaScript target for wrapped-object finalizers.
  JSValue weak_target_ = JS_UNDEFINED;

  // On the env's pending list while the finalizer has yet to run.
  napi_intrinsic_link__ pending_link_{};
  // The external array buffer made over external_data_, held weakly, while
  // the finalizer has yet to run.
  JSValue array_buffer_ = JS_UNDEFINED;
  JSNativeWeakRefLink array_buffer_link_{};
  // Teardown is detaching array_buffer_ and destroys the hint itself.
  bool detaching_ = false;
  // The array buffer let go of the memory (release_array_buffer ran).
  bool released_ = false;
};

#endif // NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_
