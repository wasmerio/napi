#ifndef NAPI_QUICKJS_AGENT_H_
#define NAPI_QUICKJS_AGENT_H_

#include "../../../include/js_native_api.h"
#include "../../../include/unofficial_napi.h"
#include "napi_allocator.h"
#include "napi_scope.h"

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <quickjs.h>

// State of one QuickJS runtime, shared by every env made on it: handle
// scopes, the pending exception, runtime-wide engine hooks and async context
// frames. An env's realm (its JSContext) lives on the env.
struct napi_agent__
{
  explicit napi_agent__(JSRuntime *runtime);
  ~napi_agent__();

  napi_agent__(const napi_agent__ &) = delete;
  napi_agent__ &operator=(const napi_agent__ &) = delete;

  JSRuntime *runtime() const;

  napi_env principal_env() const;
  void set_principal_env(napi_env env);
  // The env of the realm `ctx` belongs to, or the principal env.
  napi_env env_for_context(JSContext *ctx) const;

  // Handle scopes. Values in them are usable with every env of this agent.
  bool open_root_scope(napi_env env);
  void close_root_scope();
  void close_scopes();
  napi_handle_scope root_scope() const;
  napi_handle_scope current_scope() const;
  void set_current_scope(napi_handle_scope scope);
  bool is_current_scope(napi_handle_scope scope) const;
  napi_handle_scope create_scope(napi_env env, napi_handle_scope parent);
  void destroy_scope(napi_handle_scope scope);
  napi_scope__ *scope_from_handle(napi_handle_scope scope) const;
  bool owns_scope(napi_scope__ *scope) const;
  size_t scope_storage_slot_count() const;
  size_t active_scope_count() const;

  template <typename Fn>
  void for_each_active_scope(Fn fn) const
  {
    for (const auto &scope : scopes_)
      fn(scope);
  }

  // Pending exception, kept until N-API hands it back to JavaScript.
  bool has_pending_exception() const;
  void set_pending_exception(JSValue exception);
  JSValue take_pending_exception();
  void clear_pending_exception();

  int64_t adjust_external_memory(int64_t change_in_bytes);
  bool attach_embedder_hooks(const unofficial_napi_env_hooks &hooks);

  // Runtime-wide engine hooks. They dispatch to the env of the realm the
  // engine reports.
  void attach_promise_hook();
  void set_rejection_tracking(bool enabled);
  void clear_promise_hooks();
  void attach_module_hooks();
  void clear_module_hooks();

  // Continuation-preserved embedder data and the frame captured per promise.
  JSValueConst continuation_preserved_embedder_data() const;
  void set_continuation_preserved_embedder_data(JSValueConst value);
  void capture_context_frame(JSValueConst promise);
  void enter_context_frame(JSValueConst promise);
  void leave_context_frame(JSValueConst promise);
  void release_context_frame(JSValueConst promise);

  // Frees every JSValue the agent holds. Must run before the runtime is freed.
  void clear_values();

private:
  static void promise_hook(JSContext *ctx,
                           JSPromiseHookType type,
                           JSValueConst promise,
                           JSValueConst parent_promise,
                           void *opaque);
  static void rejection_tracker(JSContext *ctx,
                                JSValueConst promise,
                                JSValueConst reason,
                                bool is_handled,
                                void *opaque);
  static int import_meta_init(JSContext *ctx,
                              JSModuleDef *module,
                              JSValueConst meta,
                              void *opaque);
  static JSValue dynamic_import(JSContext *ctx,
                                JSModuleDef *referrer,
                                JSValueConst referrer_name,
                                JSValueConst specifier,
                                JSValueConst attributes,
                                JSModuleImportPhaseEnum phase,
                                void *opaque);

  void replace_stored_value(JSValue *target, JSValueConst value);

  JSRuntime *runtime_ = nullptr;
  napi_env principal_env_ = nullptr;

  // Handle scope stack.
  napi_allocator__<napi_scope__, napi_agent__> scopes_;
  napi_handle_scope root_scope_ = nullptr;
  napi_handle_scope current_scope_ = nullptr;

  // Pending exception.
  JSValue pending_exception_ = JS_UNDEFINED;
  bool has_pending_exception_ = false;

  // Declared external memory.
  int64_t external_memory_ = 0;

  // Embedder hooks, attached once.
  bool embedder_hooks_attached_ = false;
  unofficial_napi_env_hooks embedder_hooks_{};

  // Async context tracking.
  JSValue continuation_preserved_embedder_data_ = JS_UNDEFINED;
  std::unordered_map<void *, JSValue> promise_context_frames_;
  std::vector<JSValue> promise_context_frame_stack_;
};

#endif // NAPI_QUICKJS_AGENT_H_
