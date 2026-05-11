#ifndef NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_
#define NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"

#include <quickjs.h>

struct napi_external_backing_store_hint__
{
  static napi_external_backing_store_hint__ *create(
      napi_env env,
      void *external_data,
      node_api_basic_finalize finalize_cb,
      void *finalize_hint);
  static void destroy(napi_external_backing_store_hint__ *hint);
  static void destroy_with_runtime(JSRuntime *rt, napi_external_backing_store_hint__ *hint);

  void invoke_finalizer();
  void begin_detach();
  void end_detach();
  bool is_detaching() const;
  napi_env env() const;
  JSRuntime *runtime() const;
  void *external_data() const;
  JSValue weak_target() const;
  JSValue finalizer_target(JSValue fallback) const;
  void set_weak_target(JSValue weak_target);

private:
  napi_external_backing_store_hint__(napi_env env,
                                     void *external_data,
                                     node_api_basic_finalize finalize_cb,
                                     void *finalize_hint);
  ~napi_external_backing_store_hint__();

  napi_env env_;
  JSRuntime *rt_ = nullptr;
  void *external_data_;
  node_api_basic_finalize finalize_cb_;
  void *finalize_hint_;
  bool finalize_invoked_ = false;
  bool detaching_ = false;
  JSValue weak_target_ = JS_UNDEFINED;
};

using napi_external_backing_store_hint = napi_external_backing_store_hint__;

#endif // NAPI_QUICKJS_EXTERNAL_BACKING_STORE_HINT_H_
