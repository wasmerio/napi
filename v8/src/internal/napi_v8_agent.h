#ifndef NAPI_V8_AGENT_H_
#define NAPI_V8_AGENT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <v8.h>

#include "js_native_api.h"
#include "unofficial_napi.h"

namespace v8 {
class CpuProfiler;
}  // namespace v8

// One engine instance: a V8 isolate and the state that belongs to it rather
// than to a realm. Every env points at its agent. Today an agent has one env,
// its principal env; envs for other realms of the isolate will share it.
//
// Only the isolate's own thread reads or writes these fields, except
// `allocator` and `hash_seed`, which are fixed once the agent is set up, the
// enqueue hook, which backing-store deleters read on any thread under
// NapiV8ExternalHintsMutex(), and `termination_requested`, which any thread
// may set.
struct napi_agent__ {
  explicit napi_agent__(v8::Isolate* isolate);
  ~napi_agent__();

  napi_agent__(const napi_agent__&) = delete;
  napi_agent__& operator=(const napi_agent__&) = delete;

  // The agent installed on `isolate`, or null.
  static napi_agent__* From(v8::Isolate* isolate);

  // Turns off the agent's V8 hooks and profilers. Runs before the principal
  // env is torn down.
  void Shutdown();

  v8::Isolate* const isolate;
  napi_env principal_env = nullptr;

  // Backs the isolate's ArrayBuffers. Null when the isolate was not created
  // by unofficial_napi_create_env.
  std::shared_ptr<v8::ArrayBuffer::Allocator> allocator;
  // Provider-generated seed; zero means "ask the isolate".
  uint64_t hash_seed = 0;

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
  // The deprecated context-token hooks, called for the realms of the agent's
  // env. `context_token_data` is the hooks' `data`.
  unofficial_napi_context_token_callback context_token_assign_callback = nullptr;
  unofficial_napi_context_token_callback context_token_unassign_callback = nullptr;
  void* context_token_data = nullptr;

  // The one near-heap-limit slot. `near_heap_limit_installed` is true while
  // the provider's trampoline is registered with V8.
  unofficial_napi_near_heap_limit_callback near_heap_limit_callback = nullptr;
  void* near_heap_limit_data = nullptr;
  bool near_heap_limit_installed = false;

  // The embedder's termination: set by unofficial_napi_terminate_execution
  // until unofficial_napi_cancel_terminate_execution (or Shutdown) clears
  // it. V8 does not tell it apart from one a script timeout's watchdog asks
  // for, and forgets it on its own: once the outermost JS frame has unwound,
  // and whenever an API call enters V8 while it unwinds (V8 13.6's
  // PREPARE_FOR_EXECUTION clears it). This keeps it, so that calls into JS
  // fail until it is cancelled, as on QuickJS and as Node's can_call_into_js()
  // stays false once the env stops.
  std::atomic<bool> termination_requested{false};
  // A no-op JS function, made the first time it is needed. Called right
  // after a termination request, it makes V8 act on the request at its
  // entry, a stack check.
  v8::Global<v8::Function> termination_probe;

  // Profiler sessions. V8's CPU and sampling heap profilers are per isolate.
  v8::CpuProfiler* cpu_profiler = nullptr;
  std::vector<unofficial_napi_profile> active_profiles;
};

#endif  // NAPI_V8_AGENT_H_
