#ifndef NAPI_QUICKJS_AGENT_H_
#define NAPI_QUICKJS_AGENT_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"
#include "../../../include/unofficial_napi.h"
#include "napi_allocator.h"
#include "napi_buffer.h"
#include "napi_contextify.h"
#include "napi_deferred.h"
#include "napi_env_cleanup_hook.h"
#include "napi_error_state.h"
#include "napi_external_backing_store_hint.h"
#include "napi_module_wrap.h"
#include "napi_periodic_gate.h"
#include "napi_ref.h"
#include "napi_scope.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <quickjs.h>

// One QuickJS runtime and all N-API state: handle scopes, references, the
// pending exception and last error, instance data, cleanup hooks, promise
// and module hooks, modules, bytecode and realms. Each realm is an env that
// owns nothing but its JSContext.
struct napi_agent__
{
  explicit napi_agent__(JSRuntime *runtime);
  ~napi_agent__();

  napi_agent__(const napi_agent__ &) = delete;
  napi_agent__ &operator=(const napi_agent__ &) = delete;

  JSRuntime *runtime() const;

  // Makes the env of a new realm on `context` and takes the context. The
  // first one is the principal realm. Its global becomes findable with
  // env_for_global.
  napi_env add_realm(JSContext *context);
  napi_env principal_env() const;
  // The env of the realm `ctx` belongs to, or the principal env.
  napi_env env_for_context(JSContext *ctx) const;
  // The env of the realm whose global, or sandbox, is `object`; or null.
  napi_env env_for_global(JSValueConst object) const;
  void set_realm_global(JSValueConst object, napi_env env);

  // Runs the cleanup hooks, then frees every JSValue the agent holds and the
  // realms' contexts. Must run before the runtime is freed. Native callbacks
  // are refused from then on.
  void teardown();
  bool is_closing() const;
  // Runs after the runtime is freed, which may still finalize externals.
  void finalize_instance_data();

  // Handle scopes. Values in them are usable with every env of this agent.
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
  napi_value wrap_value_in_current_scope(JSValue value, bool owned);
  void delete_value_from_current_scope(napi_value value);
  napi_value__ *value_from_handle(napi_value value) const;

  template <typename Fn>
  void for_each_active_scope(Fn fn) const
  {
    for (const auto &scope : scopes_)
      fn(scope);
  }

  // References, and the other slots the agent allocates.
  napi_ref create_ref(napi_env env, JSValueConst value, uint32_t initial_ref_count);
  void delete_ref(napi_ref ref);
  napi_ref__ *ref_from_handle(napi_ref ref);
  size_t ref_storage_slot_count() const;
  size_t active_ref_count() const;
  napi_env_cleanup_hook__ *create_cleanup_hook(napi_env env, napi_cleanup_hook hook, void *arg);
  void destroy_cleanup_hook(napi_env_cleanup_hook__ *entry);
  napi_deferred__ *create_deferred(napi_env env, JSValue resolve, JSValue reject);
  void destroy_deferred(napi_deferred__ *deferred);
  napi_external_backing_store_hint__ *create_external_backing_store(
      napi_env env,
      void *external_data,
      node_api_basic_finalize finalize_cb,
      void *finalize_hint);
  void destroy_external_backing_store(napi_external_backing_store_hint__ *hint);

  // Pending exception, kept until N-API hands it back to JavaScript.
  bool has_pending_exception() const;
  void set_pending_exception(JSValue exception);
  JSValue take_pending_exception();
  void clear_pending_exception();

  // Last native error.
  const napi_extended_error_info *last_error_info() const;
  napi_status set_last_error(napi_status status, const char *message);
  napi_status clear_last_error();

  void *instance_data() const;
  void set_instance_data(void *data, napi_finalize finalize_cb, void *finalize_hint);

  napi_status add_cleanup_hook(napi_env env, napi_cleanup_hook hook, void *arg);
  napi_status remove_cleanup_hook(napi_cleanup_hook hook, void *arg);

  int64_t adjust_external_memory(int64_t change_in_bytes);
  bool attach_embedder_hooks(const unofficial_napi_env_hooks &hooks);

  // The promise reject callback and promise hooks (init, before, after,
  // resolve). They apply to every realm.
  napi_status set_promise_reject_callback(napi_env env, napi_value callback);
  napi_status set_promise_hooks(napi_env env, napi_value init, napi_value before,
                                napi_value after, napi_value resolve);

  // Continuation-preserved embedder data and the frame captured per promise.
  JSValueConst continuation_preserved_embedder_data() const;
  void set_continuation_preserved_embedder_data(JSValueConst value);

  napi_buffer__ &buffers();
  quickjs::detail::napi_contextify__ &contextify();
  quickjs::detail::napi_module_wrap__ &module_wrap();

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  bool should_dump_lifetime_stats(int64_t now_ms);
  bool should_dump_lifetime_string_symbol_values(int64_t now_ms);
#endif

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

  void call_hook(JSContext *ctx, JSValueConst hook, int argc, JSValueConst *argv);
  void capture_context_frame(JSValueConst promise);
  void enter_context_frame(JSValueConst promise);
  void leave_context_frame(JSValueConst promise);
  void release_context_frame(JSValueConst promise);
  void replace_stored_value(JSValue *target, JSValueConst value);

  JSRuntime *runtime_ = nullptr;

  // Realms in creation order; the first is the principal realm.
  std::vector<std::unique_ptr<napi_env__>> envs_;
  // Realm globals and sandboxes, by object identity.
  std::unordered_map<void *, napi_env> realm_globals_;

  // Handle scopes and agent-owned slots.
  napi_allocator__<napi_scope__, napi_agent__> scopes_;
  napi_handle_scope root_scope_ = nullptr;
  napi_handle_scope current_scope_ = nullptr;
  napi_allocator__<napi_ref__, napi_agent__> refs_;
  napi_allocator__<napi_env_cleanup_hook__, napi_agent__> cleanup_hook_slots_;
  napi_allocator__<napi_deferred__, napi_agent__> deferreds_;
  napi_allocator__<napi_external_backing_store_hint__, napi_agent__> external_backing_stores_;
  bool clearing_refs_ = false;

  // Pending exception and last native error.
  JSValue pending_exception_ = JS_UNDEFINED;
  bool has_pending_exception_ = false;
  napi::error_state__ error_state_;

  // Instance data and cleanup hooks (run newest first).
  void *instance_data_ = nullptr;
  napi_finalize instance_data_finalize_cb_ = nullptr;
  void *instance_data_finalize_hint_ = nullptr;
  std::vector<napi_env_cleanup_hook__ *> cleanup_hooks_;

  // Declared external memory.
  int64_t external_memory_ = 0;

  // Embedder hooks, attached once.
  bool embedder_hooks_attached_ = false;
  unofficial_napi_env_hooks embedder_hooks_{};

  // Promise reject callback and promise hooks.
  JSValue promise_reject_callback_ = JS_UNDEFINED;
  std::array<JSValue, 4> promise_hooks_{JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};

  // Async context tracking.
  JSValue continuation_preserved_embedder_data_ = JS_UNDEFINED;
  std::unordered_map<void *, JSValue> promise_context_frames_;
  std::vector<JSValue> promise_context_frame_stack_;

  // Subsystems, made with the principal realm.
  std::unique_ptr<napi_buffer__> buffers_;
  std::unique_ptr<quickjs::detail::napi_contextify__> contextify_;
  std::unique_ptr<quickjs::detail::napi_module_wrap__> module_wrap_;

  bool closing_ = false;

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  napi::periodic_gate__ lifetime_stats_gate_{2000};
  napi::periodic_gate__ lifetime_string_symbol_values_gate_{10000};
#endif
};

#endif // NAPI_QUICKJS_AGENT_H_
