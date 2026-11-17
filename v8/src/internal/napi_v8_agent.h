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
// Host memory the provider allocates for an isolate's guest, charged to the
// embedder's budget. The bridge installs it with the budget
// (snapi_private_set_host_memory); without it nothing is charged. Every charge
// holder shares it, so a charge that outlives the isolate (a queued message)
// can still return its bytes; the last holder releases the embedder's budget.
struct NapiHostMemory {
  // Budget pools, as `Pool` in src/budget.rs counts them.
  enum Pool : uint32_t {
    kBookkeeping = 0,
    kTransient = 1,
    kSerializedMessage = 2,
  };

  void* context = nullptr;
  // Nonzero if granted. A soft charge refuses without stopping the
  // application: the caller reports a catchable error.
  int (*charge)(void* context, uint32_t pool, uint64_t bytes, int soft) = nullptr;
  void (*uncharge)(void* context, uint32_t pool, uint64_t bytes) = nullptr;
  void (*release)(void* context) = nullptr;

  NapiHostMemory() = default;
  NapiHostMemory(const NapiHostMemory&) = delete;
  NapiHostMemory& operator=(const NapiHostMemory&) = delete;
  ~NapiHostMemory() {
    if (release != nullptr) release(context);
  }
};

// Bytes one provider object holds, charged to a pool of the isolate's host
// memory (if any) and returned when the holder is destroyed. Move-only.
class NapiHostCharge {
 public:
  NapiHostCharge() = default;
  NapiHostCharge(std::shared_ptr<NapiHostMemory> memory, NapiHostMemory::Pool pool)
      : memory_(std::move(memory)), pool_(pool) {}
  NapiHostCharge(const NapiHostCharge&) = delete;
  NapiHostCharge& operator=(const NapiHostCharge&) = delete;
  NapiHostCharge(NapiHostCharge&& other) noexcept
      : memory_(std::move(other.memory_)), pool_(other.pool_), bytes_(other.bytes_) {
    other.bytes_ = 0;
  }
  NapiHostCharge& operator=(NapiHostCharge&& other) noexcept {
    if (this != &other) {
      Reset();
      memory_ = std::move(other.memory_);
      pool_ = other.pool_;
      bytes_ = other.bytes_;
      other.bytes_ = 0;
    }
    return *this;
  }
  ~NapiHostCharge() { Reset(); }

  // Charges `bytes` more before they are allocated. False (nothing charged)
  // if the budget refused them; always true without host memory.
  bool Grow(uint64_t bytes, bool soft) {
    if (memory_ == nullptr || bytes == 0) return true;
    if (memory_->charge(memory_->context, pool_, bytes, soft ? 1 : 0) == 0) return false;
    bytes_ += bytes;
    return true;
  }
  // Returns `bytes` the holder no longer has.
  void Shrink(uint64_t bytes) {
    if (memory_ == nullptr || bytes == 0) return;
    if (bytes > bytes_) bytes = bytes_;
    memory_->uncharge(memory_->context, pool_, bytes);
    bytes_ -= bytes;
  }
  void Reset() {
    if (memory_ != nullptr && bytes_ != 0) memory_->uncharge(memory_->context, pool_, bytes_);
    bytes_ = 0;
  }
  uint64_t bytes() const { return bytes_; }
  bool metered() const { return memory_ != nullptr; }

 private:
  std::shared_ptr<NapiHostMemory> memory_;
  NapiHostMemory::Pool pool_ = NapiHostMemory::kTransient;
  uint64_t bytes_ = 0;
};

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
  // NapiWebAssemblyPolicy::kRestricted: no context of the isolate has the
  // WebAssembly global or compiles wasm. Fixed once the agent is set up.
  bool webassembly_restricted = false;
  // Heap space indices, resolved by name on first use: the young large-object
  // space, and the spaces the hard old-generation limit governs.
  int young_large_object_space = -1;
  std::vector<size_t> old_generation_spaces;
  bool heap_spaces_resolved = false;
  // Removes the near-heap-limit callback. The release keeps it through the
  // env's hooks and finalizers, which can run JavaScript.
  void RemoveNearHeapLimitCallback();

  // The embedder's budget for host memory the provider allocates for this
  // isolate (NapiHostMemory), or null when nothing is charged.
  std::shared_ptr<NapiHostMemory> host_memory;

  // The embedder budgets or manages the isolate's application, which shares
  // the process with others: tools that make the host write files, start
  // threads or hold unaccounted memory are refused, and the engine does not
  // consume code caches the guest supplies. Set when the isolate is created
  // (snapi_private_create_env), before anything runs in it.
  bool resource_limits = false;
  // The instance lane bound when the isolate was created
  // (snapi_v8_lane_current_id), or 0 for a standalone isolate. Fixed once the
  // agent is set up.
  uint64_t lane_id = 0;

  // Isolate-wide keys for wraps, buffer records and type tags.
  v8::Global<v8::Private> wrap_private_key;
  v8::Global<v8::Private> buffer_private_key;
  v8::Global<v8::Private> type_tag_private_key;

  // Trusted-script brand/hook installation and reusable code-like template.
  // One per engine, shared by every realm. Private::New keeps the brand
  // distinct from any key an embedder can create by name.
  v8::Global<v8::Private> trusted_script_key;
  v8::Global<v8::ObjectTemplate> trusted_script_template;
  // Includes own queues while a callback switches the current N-API realm.
  unsigned microtask_checkpoints = 0;

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

// Tracks every provider-driven checkpoint, including a realm's own queue.
// Callbacks may switch N-API realms while that queue is still draining.
class napi_microtask_checkpoint_scope__ {
 public:
  explicit napi_microtask_checkpoint_scope__(napi_agent__* agent) : agent_(agent) {
    ++agent_->microtask_checkpoints;
  }
  ~napi_microtask_checkpoint_scope__() { --agent_->microtask_checkpoints; }
  napi_microtask_checkpoint_scope__(const napi_microtask_checkpoint_scope__&) = delete;
  napi_microtask_checkpoint_scope__& operator=(const napi_microtask_checkpoint_scope__&) = delete;
 private:
  napi_agent__* agent_;
};

#endif  // NAPI_V8_AGENT_H_
