#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"
#include "../../../include/unofficial_napi.h"
#include "napi_agent.h"
#include "napi_callback_info.h"
#include "napi_buffer.h"
#include "napi_contextify.h"
#include "napi_deferred.h"
#include "napi_env_cleanup_hook.h"
#include "napi_external_backing_store_hint.h"
#include "napi_module_wrap.h"
#include "napi_promises.h"
#include "napi_ref.h"
#include "napi_scope.h"
#include "napi_value.h"
#include "napi_error_state.h"
#include "napi_periodic_gate.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <quickjs.h>

struct napi_external_backing_store_hint__;
struct napi_runtime_interrupts__;

// One realm (JSContext) of an agent. The context never changes.
struct napi_env__
{
  napi_env__(napi_agent__ *agent, JSContext *context, int32_t module_api_version);
  ~napi_env__();

  void prepare_teardown();
  void finalize_instance_data();
  // Native callbacks are refused once teardown has started.
  bool is_closing() const;

  napi_agent__ *agent() const;
  // Makes this env own its agent (an env made from an embedder's context).
  void adopt_agent(std::unique_ptr<napi_agent__> agent);
  bool is_principal() const;
  // The env whose hooks this realm falls back to; null for the principal env.
  napi_env parent() const;
  void set_parent(napi_env parent);
  JSContext *context() const;
  int32_t module_api_version() const;

  napi_handle_scope root_scope() const;
  napi_handle_scope current_scope() const;
  napi_handle_scope create_scope(napi_handle_scope parent);
  void destroy_scope(napi_handle_scope scope);
  napi_scope__ *scope_from_handle(napi_handle_scope scope) const;
  napi_value wrap_value_in_current_scope(JSValue value, bool owned);
  JSValue wrap_external_data(void *data);
  JSValue wrap_external_data(void *data, node_api_basic_finalize finalize_cb, void *finalize_hint);
  napi_value__ *value_from_handle(napi_value value);
  void delete_value_from_current_scope(napi_value value);
  napi_ref wrap_ref_in_root_scope(JSValueConst value, uint32_t initial_ref_count);
  void delete_ref_from_root_scope(napi_ref ref);
  napi_ref__ *ref_from_handle(napi_ref ref);
  size_t ref_storage_slot_count() const;
  size_t active_ref_count() const;
  bool is_current_scope(napi_handle_scope scope) const;
  void set_current_scope(napi_handle_scope scope);
  size_t scope_storage_slot_count() const;
  size_t active_scope_count() const;

  template <typename Fn>
  void for_each_active_scope(Fn fn) const
  {
    agent_->for_each_active_scope(fn);
  }

  template <typename Fn>
  void for_each_active_ref(Fn fn) const
  {
    for (const auto &ref : refs_)
      fn(ref);
  }

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  bool should_dump_lifetime_stats(int64_t now_ms);
  bool should_dump_lifetime_string_symbol_values(int64_t now_ms);
#endif

  const napi_extended_error_info *last_error_info() const;
  napi_status set_last_error(napi_status status, const char *message);
  napi_status clear_last_error();

  bool has_last_exception() const;
  bool last_exception_is_uncatchable() const;
  void clear_last_exception();
  void set_last_exception(JSValue exception);
  JSValue take_last_exception();

  // unofficial_napi_terminate_execution state, kept by the agent so that a
  // termination stops every env of it. Any thread may request termination;
  // everything else runs on the env's engine thread.
  void request_termination();
  void cancel_termination();
  bool termination_requested() const;

  void *instance_data() const;
  void set_instance_data(void *data, napi_finalize finalize_cb, void *finalize_hint);

  napi_status add_cleanup_hook(napi_cleanup_hook hook, void *arg);
  napi_status remove_cleanup_hook(napi_cleanup_hook hook, void *arg);
  napi_env_cleanup_hook__ *create_cleanup_hook(napi_cleanup_hook hook, void *arg);
  void destroy_cleanup_hook(napi_env_cleanup_hook__ *entry);
  napi_deferred__ *create_deferred(JSValue resolve, JSValue reject);
  void destroy_deferred(napi_deferred__ *deferred);
  napi_external_backing_store_hint__ *create_external_backing_store(
      void *external_data,
      node_api_basic_finalize finalize_cb,
      void *finalize_hint);
  void destroy_external_backing_store(napi_external_backing_store_hint__ *hint);
  int64_t adjust_external_memory(int64_t change_in_bytes);

  napi_promises__ &promises();
  const napi_promises__ &promises() const;
  napi_buffer__ &buffers();
  const napi_buffer__ &buffers() const;
  quickjs::detail::napi_contextify__ &contextify();
  const quickjs::detail::napi_contextify__ &contextify() const;
  quickjs::detail::napi_module_wrap__ &module_wrap();
  const quickjs::detail::napi_module_wrap__ &module_wrap() const;

  bool attach_embedder_hooks(const unofficial_napi_env_hooks &hooks);

private:
  void clear_refs_for_teardown();

  // Set only when this env owns its agent; destroyed last.
  std::unique_ptr<napi_agent__> owned_agent_;

  // Agent, realm, parent realm and API version.
  napi_agent__ *agent_;
  JSContext *context_;
  napi_env parent_ = nullptr;
  int32_t module_api_version_ = 8;

  // Last native error state.
  napi::error_state__ error_state_;

  // Termination. QuickJS polls one interrupt handler per runtime, so the
  // handler lives in a record shared by every env on this env's runtime.
  napi_runtime_interrupts__ *runtime_interrupts_ = nullptr;

  // Instance data finalizer.
  void *instance_data_ = nullptr;
  napi_finalize instance_data_finalize_cb_ = nullptr;
  void *instance_data_finalize_hint_ = nullptr;

  // Cleanup hooks in registration order.
  std::vector<napi_env_cleanup_hook__ *> env_cleanup_hooks_;

  // Env-owned slot allocators.
  napi_allocator__<napi_ref__, napi_env__> refs_;
  napi_allocator__<napi_env_cleanup_hook__, napi_env__> cleanup_hooks_;
  napi_allocator__<napi_deferred__, napi_env__> deferreds_;
  napi_allocator__<napi_external_backing_store_hint__, napi_env__> external_backing_stores_;

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  // Periodic lifetime dump scheduling.
  napi::periodic_gate__ lifetime_stats_gate_{2000};
  napi::periodic_gate__ lifetime_string_symbol_values_gate_{10000};
#endif

  // Env-owned subsystems.
  napi_buffer__ buffers_;
  napi_promises__ promises_;
  quickjs::detail::napi_contextify__ contextify_;
  quickjs::detail::napi_module_wrap__ module_wrap_;

  // Env teardown state.
  bool closing_ = false;
  bool torn_down_ = false;
  bool clearing_refs_for_teardown_ = false;
};

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message);

napi_status napi_quickjs_clear_last_error(napi_env env);

#endif // NAPI_QUICKJS_ENV_H_
