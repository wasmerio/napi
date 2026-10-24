#ifndef NAPI_V8_AGENT_H_
#define NAPI_V8_AGENT_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <v8.h>

#include "js_native_api.h"
#include "napi_error_state.h"
#include "napi_periodic_gate.h"
#include "internal/napi_factory.h"
#include "internal/napi_ref_tracker.h"
#include "unofficial_napi.h"

struct napi_buffer_record__;
struct napi_env_cleanup_hook__;
struct napi_external_backing_store_hint__;

namespace v8 {
class CpuProfiler;
}  // namespace v8

// One engine instance: a V8 isolate and every piece of N-API state. An env is
// only a realm of the agent (see napi_env__), so whichever env a call comes
// through, it reads and writes the state here.
//
// Only the isolate's own thread reads or writes these fields, except
// `allocator` and `hash_seed`, which are fixed once the agent is set up.
struct napi_agent__ {
  explicit napi_agent__(v8::Isolate* isolate);
  ~napi_agent__();

  napi_agent__(const napi_agent__&) = delete;
  napi_agent__& operator=(const napi_agent__&) = delete;

  // The agent installed on `isolate`, or null.
  static napi_agent__* From(v8::Isolate* isolate);

  // Runs cleanup hooks and finalizers and frees the N-API state, while every
  // env is still alive: finalizers get the env they were registered through.
  // Then frees the realm envs other than the principal one.
  void ReleaseState();

  // Turns off the agent's V8 hooks and profilers.
  void Shutdown();

  v8::Isolate* const isolate;
  // The first env made on the isolate. It owns the agent.
  napi_env principal_env = nullptr;
  // The other envs (realms) of this isolate, in creation order. Freed by
  // ReleaseState.
  std::vector<napi_env> realm_envs;
  int32_t module_api_version = NAPI_VERSION;

  // Backs the isolate's ArrayBuffers. Null when the isolate was not created
  // by unofficial_napi_create_env.
  std::shared_ptr<v8::ArrayBuffer::Allocator> allocator;
  // Provider-generated seed; zero means "ask the isolate".
  uint64_t hash_seed = 0;

  // The exception pending for N-API calls on the isolate's thread. All envs
  // of the agent share it, so an exception raised through one env reaches the
  // callback that called it, whichever env that callback belongs to.
  v8::Global<v8::Value> last_exception;
  v8::Global<v8::Message> last_exception_message;
  std::string last_exception_source_line;
  std::string last_exception_thrown_at;

  // Isolate-wide keys for wraps, buffer records and type tags.
  v8::Global<v8::Private> wrap_private_key;
  v8::Global<v8::Private> buffer_private_key;
  v8::Global<v8::Private> type_tag_private_key;

  // Embedder hooks from unofficial_napi_attach_env, attached once.
  bool embedder_hooks_attached = false;
  unofficial_napi_fatal_error_callback fatal_error_callback = nullptr;
  unofficial_napi_oom_error_callback oom_error_callback = nullptr;
  unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback =
      nullptr;
  void* enqueue_foreground_task_target = nullptr;

  // The one near-heap-limit slot. `near_heap_limit_installed` is true while
  // the provider's trampoline is registered with V8.
  unofficial_napi_near_heap_limit_callback near_heap_limit_callback = nullptr;
  void* near_heap_limit_data = nullptr;
  bool near_heap_limit_installed = false;

  // Profiler sessions. V8's CPU and sampling heap profilers are per isolate.
  v8::CpuProfiler* cpu_profiler = nullptr;
  std::vector<unofficial_napi_profile> active_profiles;

  // N-API state.
  napi::error_state__ error_state;
  std::vector<void*> open_handle_scope_stack;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  napi_env instance_data_env = nullptr;
  std::vector<napi_env_cleanup_hook__*> env_cleanup_hooks;
  uint64_t env_cleanup_hook_counter = 0;
  std::vector<napi_buffer_record__*> buffer_records;
  std::unordered_set<napi_external_backing_store_hint__*> external_backing_store_hints;
  napi_ref_tracker__::RefList reflist;
  napi_ref_tracker__::RefList finalizing_reflist;
  std::unordered_set<napi_ref_tracker__*> pending_finalizers;
  std::unordered_set<napi_buffer_record__*> pending_buffer_finalizers;
  bool finalization_scheduled = false;
  // unofficial_napi_set_promise_reject_callback,
  // unofficial_napi_set_prepare_stack_trace_callback and
  // unofficial_napi_set_promise_hooks (init, before, after, resolve).
  v8::Global<v8::Function> promise_reject_callback;
  v8::Global<v8::Function> prepare_stack_trace_callback;
  std::array<v8::Global<v8::Function>, 4> promise_hooks;
  // Slab storage for refs, scopes, deferreds and other N-API records.
  std::unique_ptr<v8impl::detail::napi_factory__> factory;

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  napi::periodic_gate__ lifetime_stats_gate_{2000};
  napi::periodic_gate__ lifetime_string_symbol_values_gate_{10000};
#endif
};

#endif  // NAPI_V8_AGENT_H_
