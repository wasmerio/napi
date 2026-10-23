#ifndef NAPI_QUICKJS_INTERNAL_NAPI_PROMISES_H_
#define NAPI_QUICKJS_INTERNAL_NAPI_PROMISES_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"

#include <quickjs.h>

#include <array>

// The env's promise reject callback and lifecycle hooks. The agent installs
// the runtime-wide engine hooks and dispatches to these.
class napi_promises__
{
public:
  napi_promises__(napi_env env, JSContext *context);
  ~napi_promises__();

  void teardown();

  napi_status set_reject_callback(napi_value callback);
  bool has_reject_callback() const;

  napi_status set_hooks(napi_value init,
                        napi_value before,
                        napi_value after,
                        napi_value resolve);

  void call_hook(size_t index, int argc, JSValueConst *argv);
  void call_reject_callback(JSValueConst promise, JSValueConst reason, bool is_handled);

  static JSValue microtask_job(JSContext *ctx, int argc, JSValueConst *argv);

private:
  napi_status set_optional_function(napi_value value, JSValue *target);
  void clear_stored_values();
  void call_function(JSValueConst function, int argc, JSValueConst *argv);
  void clear_pending_exception_if_any();
  bool is_active() const;

  // Owning environment and QuickJS context.
  napi_env env_ = nullptr;
  JSContext *context_ = nullptr;

  // Promise reject callback and lifecycle hooks.
  JSValue promise_reject_callback_;
  std::array<JSValue, 4> promise_hooks_;

  // Subsystem lifecycle.
  bool torn_down_ = false;
};

#endif // NAPI_QUICKJS_INTERNAL_NAPI_PROMISES_H_
