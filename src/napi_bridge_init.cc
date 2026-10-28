// N-API bridge for the WASM host.
//
// Uses unofficial_napi_create_env() from napi-v8 to obtain a proper
// napi_env with all V8 scopes managed correctly.  Each N-API function
// is wrapped with an extern "C" bridge that takes/returns u32 handle IDs
// instead of opaque pointers, so the Rust host can translate between
// WASM guest memory and native N-API calls.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NAPI_EXPERIMENTAL
#define NAPI_EXPERIMENTAL
#endif

#include "node_api.h"
#include "unofficial_napi.h"
#include "edge_v8_platform.h"
#include "internal/napi_v8_env.h"

namespace {

// Only shared registry/message ownership lives under this lock. Never enter
// V8 or invoke guest code while holding it (except documented thread-safe
// TerminateExecution/RequestInterrupt, which cannot invoke JS synchronously).
std::mutex g_mu;

struct CbRegistration {
  uint32_t guest_env;
  uint32_t wasm_fn_ptr;
  uint32_t wasm_setter_fn_ptr;
  uint64_t data_val;
};

struct CallbackInvocation {
  uint32_t id = 0;
  napi_callback_info info = nullptr;
};

struct CallbackBinding;

// Maps monotonically-increasing u32 IDs (0 = null) to opaque host handles the
// guest stores across calls. Used for the engine-owned handle kinds whose
// lifetime is not tied to a napi scope (module_wrap, bytecode).
struct HandleTable {
  std::unordered_map<uint32_t, void*> handles;
  uint32_t next_id = 1;

  uint32_t Store(void* handle) {
    if (handle == nullptr) return 0;
    const uint32_t id = next_id++;
    handles[id] = handle;
    return id;
  }
  void* Load(uint32_t id) const {
    if (id == 0) return nullptr;
    const auto it = handles.find(id);
    return it != handles.end() ? it->second : nullptr;
  }
  void* Take(uint32_t id) {
    if (id == 0) return nullptr;
    const auto it = handles.find(id);
    if (it == handles.end()) return nullptr;
    void* handle = it->second;
    handles.erase(it);
    return handle;
  }
  std::vector<void*> TakeAll() {
    std::vector<void*> result;
    result.reserve(handles.size());
    for (const auto& entry : handles) result.push_back(entry.second);
    Reset();
    return result;
  }
  void Remove(uint32_t id) { handles.erase(id); }
  void Reset() {
    handles.clear();
    next_id = 1;
  }
};

// Value IDs are generation-tagged: a u32 id encodes
// ((slot_index + 1) << kValueIdGenBits) | generation, keeping 0 as the null
// sentinel. A stale or forged id fails the generation check in LoadValue and
// resolves to null instead of aliasing whatever value later reuses the slot.
constexpr uint32_t kValueIdGenBits = 12;
constexpr uint32_t kValueIdGenMask = (1u << kValueIdGenBits) - 1;
// (slot_index + 1) must fit in the remaining 20 bits.
constexpr uint32_t kValueIdMaxSlots = (1u << 20) - 1;

struct ValueSlot {
  // Raw napi_value (a scope-bound Local). Valid only while the owning scope
  // frame is open; reclaimed (and the slot generation bumped) when it closes.
  napi_value value = nullptr;
  uint16_t generation = 0;
  bool in_use = false;
};

// One entry per open handle scope. Frame 0 is the root frame backed by the
// env-wide UnofficialEnvScope handle scope; it owns every value minted outside
// an explicit or per-callback scope and is reclaimed only at env teardown.
// Non-root frames own a real (heap) napi handle scope and the slots minted
// while they were innermost; closing the frame frees those slots so their ids
// go stale before the Locals die with the underlying scope.
struct ScopeFrame {
  napi_handle_scope scope = nullptr;
  napi_escapable_handle_scope esc_scope = nullptr;
  // The env that opened the napi scope, which must also close it. Frames are
  // shared by every env of an agent, so the env popping a frame may differ.
  napi_env env = nullptr;
  std::vector<uint32_t> owned_slots;
  // Guest-visible id (0 for the root frame and for implicit per-callback
  // frames, which the guest cannot close).
  uint32_t id = 0;
};

struct SnapiEnvState;
struct WasmInterruptRequest;

// One engine instance (isolate) and the thread driving it. Value ids, scope
// frames, refs and handle tables live here rather than on an env, so an id
// minted through one env stays valid with every other env of the same agent.
// Today each agent has exactly one env.
//
// An agent's isolate, its persistent scopes and its handle tables stay on the
// thread that created it, which owns the agent's envs (see OwnedByThisThread).
// Different agents may execute concurrently.
struct SnapiAgentState {
  // Written under g_mu by the owner when teardown starts; the owner may read
  // it without the lock.
  bool closing = false;
  // Under g_mu. Teardown runs guest finalizers, which can run JavaScript.
  // teardown_guest_calls counts guest code running from teardown; while it is
  // non-zero the napi_env and isolate are alive, so a foreign terminate can
  // still stop it. A terminate during teardown also sets stop_requested, after
  // which no more guest code starts.
  uint32_t teardown_guest_calls = 0;
  bool stop_requested = false;
  // Under g_mu. Interrupt requests the engine has queued but not run yet; the
  // engine drops them without a callback when the isolate is disposed.
  std::unordered_set<WasmInterruptRequest*> pending_interrupts;
  // Provider owner; releasing it tears down the isolate and all its envs.
  unofficial_napi_env_owner owner = nullptr;
  // Envs on this agent; the first is the principal env.
  std::vector<SnapiEnvState*> envs;

  // Value slot table: maps generation-tagged u32 IDs to scope-bound raw
  // napi_values. A value id is valid until the scope frame that owns its slot
  // closes; cross-scope retention goes through the `refs` table instead
  // (napi_create_reference / napi_get_reference_value), matching real N-API.
  std::vector<ValueSlot> value_slots;
  std::vector<uint32_t> value_free_slots;
  size_t live_value_count = 0;

  // Open scope frames, innermost last. Initialized lazily with the root frame
  // on first use; new values are minted into the innermost frame.
  std::vector<ScopeFrame> scope_frames;
  uint32_t next_scope_id = 1;
  // Emptied owned_slots buffers of popped frames. The next frame reuses one,
  // so a guest callback does not allocate and free a buffer on every call.
  std::vector<std::vector<uint32_t>> spare_owned_slots;

  // Handle table for napi_ref (references).
  std::unordered_map<uint32_t, napi_ref> refs;
  uint32_t next_ref_id = 1;

  // Handle table for napi_deferred (promise deferreds).
  std::unordered_map<uint32_t, napi_deferred> deferreds;
  uint32_t next_deferred_id = 1;

  // Handle table for opaque module_wrap handles.
  HandleTable module_wrap_handles;

  // Handle table for opaque bytecode handles (unofficial_napi_bytecode_*).
  HandleTable bytecode_handles;

  // Provider-owned profiler sessions. The provider stops any sessions still
  // active when its env is released; this table only translates pointer-sized
  // host handles to the wasm32 IDs visible to the guest.
  HandleTable profile_handles;

  // Opaque provider-owned memory leases. Unlike value slots, these survive
  // handle-scope closure and retain their JavaScript values until released.
  HandleTable buffer_lease_handles;

  // Current guest invocation driving this agent. This is only valid while the
  // driving host import remains on the stack.
  std::atomic<void*> active_callback_ctx{nullptr};
  // Guest callbacks in progress, innermost last. Callbacks nest, so entries
  // are added and removed in LIFO order and lookups hit the last few.
  std::vector<CallbackInvocation> callback_invocations;
  uint32_t next_callback_invocation_id = 1;

  // Cap on live per-value host handles / callback registrations, bounding the
  // host-side bookkeeping RSS that the byte budget pools do not otherwise see.
  // 0 means unlimited. Set from the resource budget at env creation.
  size_t value_limit = 0;
};

// One N-API env. The guest's env id resolves to this; everything it shares
// with the other envs of its engine instance is on `agent`.
struct SnapiEnvState {
  SnapiAgentState* agent = nullptr;
  napi_env env = nullptr;
  // One reference held by the bridge until release, plus one per
  // snapi_bridge_unofficial_retain_env. A retained pointer may be handed to
  // other threads: it can outlive the env and its agent but is never reused
  // for another env.
  std::atomic<uint32_t> holders{1};

  // Guest callbacks registered through this env. Each entry records the guest
  // env id the callback is invoked with, so they belong to the env.
  std::unordered_map<uint32_t, CbRegistration> cb_registry;
  uint32_t next_cb_reg_id = 1;
  std::vector<std::unique_ptr<CallbackBinding>> callback_bindings;
};

struct CallbackBinding {
  SnapiEnvState* state = nullptr;
  uint32_t reg_id = 0;
};

// The value limit is per agent, so registrations count across its envs.
size_t AgentCallbackRegistrations(const SnapiAgentState& agent) {
  size_t count = 0;
  for (const SnapiEnvState* env_state : agent.envs) count += env_state->cb_registry.size();
  return count;
}

size_t AgentCallbackBindings(const SnapiAgentState& agent) {
  size_t count = 0;
  for (const SnapiEnvState* env_state : agent.envs) {
    count += env_state->callback_bindings.size();
  }
  return count;
}

// Never destroyed: static destructors race concurrent env teardown at process
// exit (see the runtime-globals comment in unofficial_napi.cc).
std::unordered_set<SnapiEnvState*>& g_envs = *new std::unordered_set<SnapiEnvState*>();

// Envs created on this thread and not yet released. Only this thread inserts
// or erases its entries, so membership proves both that an env is alive and
// that the caller owns it, without taking g_mu on every N-API call. A plain
// pointer (freed when the thread's last env goes) keeps it usable from other
// thread-exit destructors that may still release envs.
thread_local std::unordered_set<SnapiEnvState*>* t_owned_envs = nullptr;
// The env this thread resolved last, always a member of t_owned_envs. A
// thread usually drives one env, so this answers most lookups without
// hashing.
thread_local SnapiEnvState* t_last_owned_env = nullptr;

bool OwnedByThisThread(const SnapiEnvState* state) {
  if (state == nullptr) return false;
  if (state == t_last_owned_env) return true;
  if (t_owned_envs == nullptr ||
      t_owned_envs->count(const_cast<SnapiEnvState*>(state)) == 0) {
    return false;
  }
  t_last_owned_env = const_cast<SnapiEnvState*>(state);
  return true;
}

void AdoptOnThisThread(SnapiEnvState* state) {
  if (t_owned_envs == nullptr) {
    t_owned_envs = new std::unordered_set<SnapiEnvState*>();
  }
  t_owned_envs->insert(state);
}

void ForgetOnThisThread(SnapiEnvState* state) {
  if (t_last_owned_env == state) t_last_owned_env = nullptr;
  if (t_owned_envs == nullptr) return;
  t_owned_envs->erase(state);
  if (t_owned_envs->empty()) {
    delete t_owned_envs;
    t_owned_envs = nullptr;
  }
}

struct WasmInterruptRequest {
  SnapiEnvState* state;
  uint32_t guest_env;
  uint32_t wasm_fn_ptr;
  uint32_t callback_arg;
};

void DropEnvStateHolder(SnapiEnvState* state) {
  if (state->holders.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state;
}

// Brackets guest code invoked by the bridge on the owner thread. While the
// env's agent is being released, it keeps the agent stoppable (see
// teardown_guest_calls) and refuses to start guest code once a stop was
// requested.
class GuestCallScope {
 public:
  explicit GuestCallScope(SnapiEnvState* state)
      : agent_(state != nullptr ? state->agent : nullptr) {
    if (agent_ == nullptr || !agent_->closing) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (agent_->stop_requested) {
      allowed_ = false;
      return;
    }
    ++agent_->teardown_guest_calls;
    counted_ = true;
  }
  ~GuestCallScope() {
    if (!counted_) return;
    std::lock_guard<std::mutex> lock(g_mu);
    --agent_->teardown_guest_calls;
  }
  GuestCallScope(const GuestCallScope&) = delete;
  GuestCallScope& operator=(const GuestCallScope&) = delete;
  bool allowed() const { return allowed_; }

 private:
  SnapiAgentState* agent_;
  bool allowed_ = true;
  bool counted_ = false;
};
// Message resources cross environment boundaries, so their guest-visible IDs
// cannot belong to either the source or destination environment. The table is
// process-wide and each entry is removed atomically by take or drop.
HandleTable& g_message_handles = *new HandleTable();

CallbackBinding* RegisterCallbackBinding(SnapiEnvState* state, uint32_t reg_id) {
  if (state == nullptr || reg_id == 0) return nullptr;
  const SnapiAgentState& agent = *state->agent;
  if (agent.value_limit != 0 &&
      AgentCallbackBindings(agent) >= agent.value_limit) {
    return nullptr;
  }
  state->callback_bindings.push_back(
      std::make_unique<CallbackBinding>(CallbackBinding{state, reg_id}));
  return state->callback_bindings.back().get();
}

SnapiEnvState* LookupEnvState(SnapiEnvState* env_state) {
  if (env_state == nullptr) return nullptr;
  // The pointer originates from the guest (via the Rust env-id layer); gate it
  // on live-env membership before the first dereference so a stale or forged
  // env handle fails cleanly instead of touching freed memory. Only envs this
  // thread owns are usable here; other threads get null.
  if (!OwnedByThisThread(env_state) || env_state->env == nullptr) return nullptr;
  return env_state;
}

uint32_t RegisterCallbackInvocation(SnapiEnvState* state, napi_callback_info info) {
  if (state == nullptr || info == nullptr) return 0;
  SnapiAgentState& agent = *state->agent;
  uint32_t id = agent.next_callback_invocation_id++;
  if (id == 0) id = agent.next_callback_invocation_id++;
  agent.callback_invocations.push_back(CallbackInvocation{id, info});
  return id;
}

// The helpers below take the env the guest called through and work on that
// env's agent, which owns the value, scope, ref and handle tables.

napi_callback_info FindCallbackInvocation(const SnapiEnvState& state, uint32_t id) {
  const auto& invocations = state.agent->callback_invocations;
  for (auto it = invocations.rbegin(); it != invocations.rend(); ++it) {
    if (it->id == id) return it->info;
  }
  return nullptr;
}

// Innermost open frame, creating the root frame on first use.
ScopeFrame& CurrentFrame(SnapiEnvState& state) {
  SnapiAgentState& agent = *state.agent;
  if (agent.scope_frames.empty()) {
    agent.scope_frames.emplace_back();
  }
  return agent.scope_frames.back();
}

// Pushes the frame for a napi scope `state` just opened, above the root
// frame. The frame records that env, which must also close the scope. It
// reuses a popped frame's buffer, so a callback doesn't allocate one per call.
ScopeFrame& PushScopeFrame(SnapiEnvState& state, napi_handle_scope scope,
                           napi_escapable_handle_scope esc_scope) {
  (void)CurrentFrame(state);
  SnapiAgentState& agent = *state.agent;
  ScopeFrame& frame = agent.scope_frames.emplace_back();
  frame.scope = scope;
  frame.esc_scope = esc_scope;
  frame.env = state.env;
  if (!agent.spare_owned_slots.empty()) {
    frame.owned_slots = std::move(agent.spare_owned_slots.back());
    agent.spare_owned_slots.pop_back();
  }
  return frame;
}

// Mints an id for `val` owned by `frame`. The common path stores into the
// innermost frame; escape_handle stores into the parent of the escaped scope.
uint32_t StoreValueInFrame(SnapiEnvState& state, napi_value val, ScopeFrame& frame) {
  if (val == nullptr) return 0;
  if (state.env == nullptr) return 0;
  SnapiAgentState& agent = *state.agent;
  // Refuse once the per-agent handle cap is reached: returning 0 surfaces as
  // an N-API failure the guest can handle, instead of leaking host RSS
  // unbounded.
  if (agent.value_limit != 0 && agent.live_value_count >= agent.value_limit) {
    return 0;
  }
  uint32_t index;
  if (!agent.value_free_slots.empty()) {
    index = agent.value_free_slots.back();
    agent.value_free_slots.pop_back();
  } else {
    if (agent.value_slots.size() >= kValueIdMaxSlots) return 0;
    index = static_cast<uint32_t>(agent.value_slots.size());
    agent.value_slots.emplace_back();
  }
  ValueSlot& slot = agent.value_slots[index];
  slot.value = val;
  slot.in_use = true;
  ++agent.live_value_count;
  frame.owned_slots.push_back(index);
  return ((index + 1) << kValueIdGenBits) | slot.generation;
}

uint32_t StoreValue(SnapiEnvState& state, napi_value val) {
  return StoreValueInFrame(state, val, CurrentFrame(state));
}

napi_value LoadValue(SnapiEnvState& state, uint32_t id) {
  if (id == 0) return nullptr;
  if (state.env == nullptr) return nullptr;
  SnapiAgentState& agent = *state.agent;
  const uint32_t index_plus_one = id >> kValueIdGenBits;
  if (index_plus_one == 0 || index_plus_one > agent.value_slots.size()) {
    return nullptr;
  }
  ValueSlot& slot = agent.value_slots[index_plus_one - 1];
  if (!slot.in_use || slot.generation != (id & kValueIdGenMask) ||
      slot.value == nullptr) {
    return nullptr;
  }
  return slot.value;
}

// Releases a slot and bumps its generation so any id minted for the old
// occupant is rejected by LoadValue. The Local itself dies when the owning
// napi scope closes; this only retires the id.
void FreeValueSlot(SnapiAgentState& agent, uint32_t index) {
  ValueSlot& slot = agent.value_slots[index];
  if (!slot.in_use) return;
  slot.value = nullptr;
  slot.in_use = false;
  slot.generation = static_cast<uint16_t>((slot.generation + 1) & kValueIdGenMask);
  agent.value_free_slots.push_back(index);
  --agent.live_value_count;
}

// Frees every slot the innermost frame owns and pops it. Closes the frame's
// napi scope (which kills the backing Locals) unless `close_napi_scope` is
// false (env-teardown drain, where release_env reclaims the whole env).
void PopCurrentFrame(SnapiEnvState& state, bool close_napi_scope) {
  SnapiAgentState& agent = *state.agent;
  if (agent.scope_frames.empty()) return;
  ScopeFrame frame = std::move(agent.scope_frames.back());
  agent.scope_frames.pop_back();
  for (uint32_t index : frame.owned_slots) {
    FreeValueSlot(agent, index);
  }
  // Keep a few small buffers for the next frames; drop large ones so one
  // big scope does not pin its memory for the agent's lifetime.
  constexpr size_t kMaxSpareFrames = 8;
  constexpr size_t kMaxSpareCapacity = 256;
  if (agent.spare_owned_slots.size() < kMaxSpareFrames &&
      frame.owned_slots.capacity() != 0 &&
      frame.owned_slots.capacity() <= kMaxSpareCapacity) {
    frame.owned_slots.clear();
    agent.spare_owned_slots.push_back(std::move(frame.owned_slots));
  }
  if (close_napi_scope && state.env != nullptr && frame.env != nullptr) {
    if (frame.esc_scope != nullptr) {
      (void)napi_close_escapable_handle_scope(frame.env, frame.esc_scope);
    } else if (frame.scope != nullptr) {
      (void)napi_close_handle_scope(frame.env, frame.scope);
    }
  }
}

uint64_t BackingStoreToken(const std::shared_ptr<v8::BackingStore>& backing_store) {
  return backing_store == nullptr
             ? 0
             : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(backing_store.get()));
}

uint64_t napi_v8_get_arraybuffer_backing_store_token(napi_env env, napi_value value) {
  (void)env;
  if (value == nullptr) return 0;

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsArrayBuffer()) return 0;

  return BackingStoreToken(raw.As<v8::ArrayBuffer>()->GetBackingStore());
}

uint64_t napi_v8_get_arraybuffer_view_backing_store_token(napi_env env, napi_value value) {
  (void)env;
  if (value == nullptr) return 0;

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsArrayBufferView()) return 0;

  return BackingStoreToken(raw.As<v8::ArrayBufferView>()->Buffer()->GetBackingStore());
}

size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
  }
  return 0;
}

uint32_t StoreRef(SnapiEnvState& state, napi_ref ref) {
  if (ref == nullptr) return 0;
  SnapiAgentState& agent = *state.agent;
  uint32_t id = agent.next_ref_id++;
  agent.refs[id] = ref;
  return id;
}

napi_ref LoadRef(SnapiEnvState& state, uint32_t id) {
  if (id == 0) return nullptr;
  const SnapiAgentState& agent = *state.agent;
  auto it = agent.refs.find(id);
  return it != agent.refs.end() ? it->second : nullptr;
}

void RemoveRef(SnapiEnvState& state, uint32_t id) {
  state.agent->refs.erase(id);
}

uint32_t StoreDeferred(SnapiEnvState& state, napi_deferred d) {
  if (d == nullptr) return 0;
  SnapiAgentState& agent = *state.agent;
  uint32_t id = agent.next_deferred_id++;
  agent.deferreds[id] = d;
  return id;
}

napi_deferred LoadDeferred(SnapiEnvState& state, uint32_t id) {
  if (id == 0) return nullptr;
  const SnapiAgentState& agent = *state.agent;
  auto it = agent.deferreds.find(id);
  return it != agent.deferreds.end() ? it->second : nullptr;
}

void RemoveDeferred(SnapiEnvState& state, uint32_t id) {
  state.agent->deferreds.erase(id);
}

// Finds the open frame with the given guest-visible id (innermost first).
int FindFrameById(SnapiEnvState& state, uint32_t scope_id) {
  if (scope_id == 0) return -1;
  const std::vector<ScopeFrame>& frames = state.agent->scope_frames;
  for (int i = static_cast<int>(frames.size()) - 1; i >= 0; --i) {
    if (frames[i].id == scope_id) return i;
  }
  return -1;
}

uint32_t StoreModuleWrapHandle(SnapiEnvState& state, unofficial_napi_module module) {
  return state.agent->module_wrap_handles.Store(reinterpret_cast<void*>(module));
}

unofficial_napi_module LoadModuleWrapHandle(SnapiEnvState& state, uint32_t id) {
  return reinterpret_cast<unofficial_napi_module>(state.agent->module_wrap_handles.Load(id));
}

void RemoveModuleWrapHandle(SnapiEnvState& state, uint32_t id) {
  state.agent->module_wrap_handles.Remove(id);
}

uint32_t StoreBytecodeHandle(SnapiEnvState& state, unofficial_napi_bytecode handle) {
  return state.agent->bytecode_handles.Store(reinterpret_cast<void*>(handle));
}

unofficial_napi_bytecode LoadBytecodeHandle(SnapiEnvState& state, uint32_t id) {
  return reinterpret_cast<unofficial_napi_bytecode>(state.agent->bytecode_handles.Load(id));
}

void RemoveBytecodeHandle(SnapiEnvState& state, uint32_t id) {
  state.agent->bytecode_handles.Remove(id);
}

SnapiEnvState* RequireEnvState(SnapiEnvState* env_state) {
  auto* bridge_state = LookupEnvState(env_state);
  if (bridge_state == nullptr || bridge_state->env == nullptr) {
    return nullptr;
  }
  return bridge_state;
}

// Tears down the agent `state` belongs to, with every env on it: release_env
// takes the provider owner, which disposes the whole isolate.
napi_status DisposeBridgeState(SnapiEnvState* state) {
  if (state == nullptr) return napi_ok;
  if (!OwnedByThisThread(state)) return napi_invalid_arg;
  SnapiAgentState* agent = state->agent;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (agent->closing) return napi_invalid_arg;
    agent->closing = true;
  }
  // Owner callbacks/finalizers may use their env while it unwinds. Foreign
  // interrupts are now refused; a foreign terminate only reaches guest code
  // that teardown runs (GuestCallScope). No registry lock spans reference
  // deletion, finalizers, V8 shutdown or background-task draining.
  napi_env env = state->env;
  for (const auto& entry : agent->buffer_lease_handles.handles) {
    (void)unofficial_napi_release_buffer_lease(
        env, static_cast<unofficial_napi_buffer_lease>(entry.second), false);
  }
  agent->buffer_lease_handles.Reset();
  // Slots hold raw Locals; they die with the env's scopes when release_env
  // tears everything down, so draining the frames is pure bookkeeping.
  while (!agent->scope_frames.empty()) {
    PopCurrentFrame(*state, /*close_napi_scope=*/true);
  }
  agent->value_slots.clear();
  agent->value_free_slots.clear();
  agent->live_value_count = 0;
  agent->next_scope_id = 1;
  agent->refs.clear();
  agent->next_ref_id = 1;
  agent->deferreds.clear();
  agent->next_deferred_id = 1;
  agent->module_wrap_handles.Reset();
  // Bytecode handles own engine resources outside any napi scope (V8
  // Globals / QuickJS JSValue refcounts), so they must be released
  // explicitly — env teardown does not reclaim them.
  for (auto& entry : agent->bytecode_handles.handles) {
    if (entry.second != nullptr) {
      (void)unofficial_napi_bytecode_release(
          env, reinterpret_cast<unofficial_napi_bytecode>(entry.second));
    }
  }
  agent->bytecode_handles.Reset();
  agent->profile_handles.Reset();
  agent->callback_invocations.clear();
  agent->next_callback_invocation_id = 1;
  for (SnapiEnvState* env_state : agent->envs) {
    env_state->cb_registry.clear();
    env_state->next_cb_reg_id = 1;
  }
  napi_status s = napi_ok;
  if (agent->owner != nullptr) {
    s = unofficial_napi_release_env(agent->owner, nullptr);
    agent->owner = nullptr;
  }
  // JS functions can still call their bindings while finalizers run above
  // (with no registry entry they return undefined), so free them only now.
  for (SnapiEnvState* env_state : agent->envs) env_state->callback_bindings.clear();
  agent->active_callback_ctx.store(nullptr, std::memory_order_release);
  std::unordered_set<WasmInterruptRequest*> unserviced;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (SnapiEnvState* env_state : agent->envs) {
      env_state->env = nullptr;
      g_envs.erase(env_state);
    }
    unserviced.swap(agent->pending_interrupts);
  }
  // The isolate is gone, so these callbacks can no longer run.
  for (auto* request : unserviced) delete request;
  // A retained env pointer may outlive its agent; nothing reaches the agent
  // through it once the env has left g_envs and this thread's owned set.
  for (SnapiEnvState* env_state : agent->envs) {
    env_state->agent = nullptr;
    ForgetOnThisThread(env_state);
    DropEnvStateHolder(env_state);
  }
  delete agent;
  return s;
}

// Creates an agent for a freshly created provider env and registers that env
// as its principal env, owned by this thread. On failure the provider env is
// released.
SnapiEnvState* NewAgentWithPrincipalEnv(napi_env env, unofficial_napi_env_owner owner) {
  auto* agent = new (std::nothrow) SnapiAgentState();
  auto* state = new (std::nothrow) SnapiEnvState();
  if (agent == nullptr || state == nullptr) {
    delete agent;
    delete state;
    (void)unofficial_napi_release_env(owner, nullptr);
    return nullptr;
  }
  agent->owner = owner;
  agent->envs.push_back(state);
  state->agent = agent;
  state->env = env;
  { std::lock_guard<std::mutex> lock(g_mu); g_envs.insert(state); }
  AdoptOnThisThread(state);
  return state;
}

}  // namespace

// ============================================================
// Initialization
// ============================================================

// Host-only: sizes the process-wide V8 background worker pool. Not reachable
// from guest code -- a guest must not get to choose how many threads the host
// runs. Returns 0 once the platform exists, since the pool is fixed then.
extern "C" int snapi_bridge_set_v8_worker_thread_count(uint32_t count) {
  return EdgeV8Platform::SetWorkerThreadCount(static_cast<int>(count)) ? 1 : 0;
}

extern "C" int snapi_bridge_init() {
  // Intentionally do not create a N-API env here.
  // Env creation is deferred until the guest explicitly calls
  // `unofficial_napi_create_env`, so init happens on the execution thread.
  return 1;
}

// ============================================================
// Value creation
// ============================================================

extern "C" int snapi_bridge_get_undefined(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_get_undefined(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_null(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_get_null(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_boolean(SnapiEnvState* env_state, int value, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_get_boolean(env, value != 0, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_global(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_get_global(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_string_utf8(SnapiEnvState* env_state, const char* str,
                                               uint32_t wasm_length,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_utf8(env, str, length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_string_latin1(SnapiEnvState* env_state, const char* str,
                                                 uint32_t wasm_length,
                                                 uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_latin1(env, str, length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_int32(SnapiEnvState* env_state, int32_t value, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_int32(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_uint32(SnapiEnvState* env_state, uint32_t value, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_uint32(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_double(SnapiEnvState* env_state, double value, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_double(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_int64(SnapiEnvState* env_state, int64_t value, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_int64(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_object(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_object(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_array(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_array(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_array_with_length(SnapiEnvState* env_state, uint32_t length,
                                                     uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_array_with_length(env, (size_t)length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Value reading
// ============================================================

extern "C" int snapi_bridge_get_value_string_utf8(SnapiEnvState* env_state, uint32_t id, char* buf,
                                                  size_t bufsize,
                                                  size_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_utf8(env, val, buf, bufsize, result);
}

extern "C" int snapi_bridge_get_value_string_latin1(SnapiEnvState* env_state, uint32_t id, char* buf,
                                                    size_t bufsize,
                                                    size_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_latin1(env, val, buf, bufsize, result);
}

extern "C" int snapi_bridge_get_value_int32(SnapiEnvState* env_state, uint32_t id, int32_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_int32(env, val, result);
}

extern "C" int snapi_bridge_get_value_uint32(SnapiEnvState* env_state, uint32_t id, uint32_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_uint32(env, val, result);
}

extern "C" int snapi_bridge_get_value_double(SnapiEnvState* env_state, uint32_t id, double* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_double(env, val, result);
}

extern "C" int snapi_bridge_get_value_int64(SnapiEnvState* env_state, uint32_t id, int64_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_int64(env, val, result);
}

extern "C" int snapi_bridge_get_value_bool(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool b;
  napi_status s = napi_get_value_bool(env, val, &b);
  if (s != napi_ok) return s;
  *result = b ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Type checking
// ============================================================

extern "C" int snapi_bridge_typeof(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_valuetype vtype;
  napi_status s = napi_typeof(env, val, &vtype);
  if (s != napi_ok) return s;
  *result = (int)vtype;
  return napi_ok;
}

extern "C" int snapi_bridge_is_array(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_array(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_error(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_error(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_arraybuffer(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_arraybuffer(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_typedarray(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_typedarray(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_dataview(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_dataview(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_date(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_date(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_promise(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_promise(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_instanceof(SnapiEnvState* env_state, uint32_t obj_id, uint32_t ctor_id,
                                       int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value ctor = LoadValue(*bridge_state, ctor_id);
  if (!obj || !ctor) return napi_invalid_arg;
  bool is;
  napi_status s = napi_instanceof(env, obj, ctor, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Coercion
// ============================================================

extern "C" int snapi_bridge_coerce_to_bool(SnapiEnvState* env_state, uint32_t id, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_bool(env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_number(SnapiEnvState* env_state, uint32_t id, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_number(env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_string(SnapiEnvState* env_state, uint32_t id, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_string(env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_object(SnapiEnvState* env_state, uint32_t id, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_object(env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Object operations
// ============================================================

extern "C" int snapi_bridge_set_property(SnapiEnvState* env_state, uint32_t obj_id, uint32_t key_id,
                                         uint32_t val_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value key = LoadValue(*bridge_state, key_id);
  napi_value val = LoadValue(*bridge_state, val_id);
  if (!obj || !key || !val) return napi_invalid_arg;
  return napi_set_property(env, obj, key, val);
}

extern "C" int snapi_bridge_get_property(SnapiEnvState* env_state, uint32_t obj_id, uint32_t key_id,
                                         uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value key = LoadValue(*bridge_state, key_id);
  if (!obj || !key) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_property(env, obj, key, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_property(SnapiEnvState* env_state, uint32_t obj_id, uint32_t key_id,
                                         int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value key = LoadValue(*bridge_state, key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_property(env, obj, key, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_has_own_property(SnapiEnvState* env_state, uint32_t obj_id, uint32_t key_id,
                                             int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value key = LoadValue(*bridge_state, key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_own_property(env, obj, key, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_delete_property(SnapiEnvState* env_state, uint32_t obj_id, uint32_t key_id,
                                            int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value key = LoadValue(*bridge_state, key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool deleted;
  napi_status s = napi_delete_property(env, obj, key, &deleted);
  if (s != napi_ok) return s;
  *result = deleted ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_set_named_property(SnapiEnvState* env_state, uint32_t obj_id,
                                               const char* name,
                                               uint32_t val_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value val = LoadValue(*bridge_state, val_id);
  if (!obj || !val || !name) return napi_invalid_arg;
  return napi_set_named_property(env, obj, name, val);
}

extern "C" int snapi_bridge_get_named_property(SnapiEnvState* env_state, uint32_t obj_id,
                                               const char* name,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj || !name) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_named_property(env, obj, name, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_named_property(SnapiEnvState* env_state, uint32_t obj_id,
                                               const char* name,
                                               int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj || !name) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_named_property(env, obj, name, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_set_element(SnapiEnvState* env_state, uint32_t obj_id, uint32_t index,
                                        uint32_t val_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  napi_value val = LoadValue(*bridge_state, val_id);
  if (!obj || !val) return napi_invalid_arg;
  return napi_set_element(env, obj, index, val);
}

extern "C" int snapi_bridge_get_element(SnapiEnvState* env_state, uint32_t obj_id, uint32_t index,
                                        uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_element(env, obj, index, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_element(SnapiEnvState* env_state, uint32_t obj_id, uint32_t index,
                                        int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_element(env, obj, index, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_delete_element(SnapiEnvState* env_state, uint32_t obj_id, uint32_t index,
                                           int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  bool deleted;
  napi_status s = napi_delete_element(env, obj, index, &deleted);
  if (s != napi_ok) return s;
  *result = deleted ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_array_length(SnapiEnvState* env_state, uint32_t arr_id,
                                             uint32_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value arr = LoadValue(*bridge_state, arr_id);
  if (!arr) return napi_invalid_arg;
  return napi_get_array_length(env, arr, result);
}

extern "C" int snapi_bridge_get_property_names(SnapiEnvState* env_state, uint32_t obj_id,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_property_names(env, obj, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_all_property_names(SnapiEnvState* env_state, uint32_t obj_id,
                                                   int mode, int filter,
                                                   int conversion,
                                                   uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_all_property_names(
      env, obj, (napi_key_collection_mode)mode, (napi_key_filter)filter,
      (napi_key_conversion)conversion, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_prototype(SnapiEnvState* env_state, uint32_t obj_id, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_prototype(env, obj, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_object_freeze(SnapiEnvState* env_state, uint32_t obj_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  return napi_object_freeze(env, obj);
}

extern "C" int snapi_bridge_object_seal(SnapiEnvState* env_state, uint32_t obj_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  return napi_object_seal(env, obj);
}

// ============================================================
// Comparison
// ============================================================

extern "C" int snapi_bridge_strict_equals(SnapiEnvState* env_state, uint32_t a_id, uint32_t b_id,
                                          int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value a = LoadValue(*bridge_state, a_id);
  napi_value b = LoadValue(*bridge_state, b_id);
  if (!a || !b) return napi_invalid_arg;
  bool eq;
  napi_status s = napi_strict_equals(env, a, b, &eq);
  if (s != napi_ok) return s;
  *result = eq ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Error handling
// ============================================================

extern "C" int snapi_bridge_create_error(SnapiEnvState* env_state, uint32_t code_id, uint32_t msg_id,
                                         uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value code = LoadValue(*bridge_state, code_id);  // can be null (0)
  napi_value msg = LoadValue(*bridge_state, msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_error(env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_type_error(SnapiEnvState* env_state, uint32_t code_id,
                                              uint32_t msg_id,
                                              uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value code = LoadValue(*bridge_state, code_id);
  napi_value msg = LoadValue(*bridge_state, msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_type_error(env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_range_error(SnapiEnvState* env_state, uint32_t code_id,
                                               uint32_t msg_id,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value code = LoadValue(*bridge_state, code_id);
  napi_value msg = LoadValue(*bridge_state, msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_range_error(env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_throw(SnapiEnvState* env_state, uint32_t error_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value error = LoadValue(*bridge_state, error_id);
  if (!error) return napi_invalid_arg;
  return napi_throw(env, error);
}

extern "C" int snapi_bridge_throw_error(SnapiEnvState* env_state, const char* code, const char* msg) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return napi_throw_error(env, code, msg);
}

extern "C" int snapi_bridge_throw_type_error(SnapiEnvState* env_state, const char* code,
                                             const char* msg) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return napi_throw_type_error(env, code, msg);
}

extern "C" int snapi_bridge_throw_range_error(SnapiEnvState* env_state, const char* code,
                                              const char* msg) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return napi_throw_range_error(env, code, msg);
}

extern "C" int snapi_bridge_is_exception_pending(SnapiEnvState* env_state, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  bool pending;
  napi_status s = napi_is_exception_pending(env, &pending);
  if (s != napi_ok) return s;
  *result = pending ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_and_clear_last_exception(SnapiEnvState* env_state, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_get_and_clear_last_exception(env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Symbol
// ============================================================

extern "C" int snapi_bridge_create_symbol(SnapiEnvState* env_state, uint32_t description_id,
                                          uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value description = LoadValue(*bridge_state, description_id);  // can be null (0)
  napi_value result;
  napi_status s = napi_create_symbol(env, description, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// BigInt
// ============================================================

extern "C" int snapi_bridge_create_bigint_int64(SnapiEnvState* env_state, int64_t value,
                                                uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_bigint_int64(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_bigint_uint64(SnapiEnvState* env_state, uint64_t value,
                                                 uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_bigint_uint64(env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_int64(SnapiEnvState* env_state, uint32_t id, int64_t* value,
                                                   int* lossless) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool loss;
  napi_status s = napi_get_value_bigint_int64(env, val, value, &loss);
  if (s != napi_ok) return s;
  *lossless = loss ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_uint64(SnapiEnvState* env_state, uint32_t id,
                                                    uint64_t* value,
                                                    int* lossless) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool loss;
  napi_status s = napi_get_value_bigint_uint64(env, val, value, &loss);
  if (s != napi_ok) return s;
  *lossless = loss ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Date
// ============================================================

extern "C" int snapi_bridge_create_date(SnapiEnvState* env_state, double time, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_date(env, time, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_date_value(SnapiEnvState* env_state, uint32_t id, double* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_date_value(env, val, result);
}

// ============================================================
// Promise
// ============================================================

extern "C" int snapi_bridge_create_promise(SnapiEnvState* env_state, uint32_t* deferred_out,
                                           uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_deferred deferred;
  napi_value promise;
  napi_status s = napi_create_promise(env, &deferred, &promise);
  if (s != napi_ok) return s;
  *deferred_out = StoreDeferred(*bridge_state, deferred);
  *out_id = StoreValue(*bridge_state, promise);
  return napi_ok;
}

extern "C" int snapi_bridge_resolve_deferred(SnapiEnvState* env_state, uint32_t deferred_id,
                                             uint32_t value_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_deferred d = LoadDeferred(*bridge_state, deferred_id);
  napi_value val = LoadValue(*bridge_state, value_id);
  if (!d || !val) return napi_invalid_arg;
  napi_status s = napi_resolve_deferred(env, d, val);
  if (s == napi_ok) RemoveDeferred(*bridge_state, deferred_id);
  return s;
}

extern "C" int snapi_bridge_reject_deferred(SnapiEnvState* env_state, uint32_t deferred_id,
                                            uint32_t value_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_deferred d = LoadDeferred(*bridge_state, deferred_id);
  napi_value val = LoadValue(*bridge_state, value_id);
  if (!d || !val) return napi_invalid_arg;
  napi_status s = napi_reject_deferred(env, d, val);
  if (s == napi_ok) RemoveDeferred(*bridge_state, deferred_id);
  return s;
}

// ============================================================
// ArrayBuffer
// ============================================================

extern "C" int snapi_bridge_create_arraybuffer(SnapiEnvState* env_state, uint32_t byte_length,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data;
  napi_value result;
  napi_status s =
      napi_create_arraybuffer(env, (size_t)byte_length, &data, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// Rust-exported finalizer for guest-heap-backed buffers: frees the allocation
// back to the guest heap and drops the boxed hint. Weak no-op fallback keeps
// embedders without the Rust host linking (they never create finalized
// externals).
extern "C" __attribute__((weak)) void napi_host_guest_heap_buffer_finalize(
    void* /*data*/, void* /*hint*/) {}

namespace {
void GuestHeapBufferFinalizeTrampoline(node_api_basic_env /*env*/, void* data,
                                       void* hint) {
  napi_host_guest_heap_buffer_finalize(data, hint);
}
}  // namespace

// Forward-declare the Rust finalizer trampoline (defined in guest/callback.rs
// via #[no_mangle]). Re-enters the guest and dispatches the guest's finalize
// callback through the indirect function table.
extern "C" uint32_t snapi_host_invoke_wasm_finalizer(void* callback_ctx,
                                                     uint32_t guest_env,
                                                     uint32_t wasm_fn_ptr,
                                                     uint32_t data,
                                                     uint32_t hint);

namespace {
// Describes a guest-registered N-API finalizer. Carried as the `finalize_hint`
// of a host finalizer installed via napi_wrap / napi_add_finalizer, all of
// which fire on the *deferred* finalizer drain: their RefBase weak callbacks
// enqueue into pending_finalizers, drained from the guest's
// process-microtasks import call where an active callback ctx lets us re-enter
// the guest. (External array buffers/buffers deliberately do NOT use V8's
// backing-store deleter for the guest finalizer — those run synchronously in
// GC, with no way back into the guest — they attach via napi_add_finalizer
// instead.) Allocated on registration, freed exactly once when it fires.
struct GuestFinalizerRecord {
  SnapiEnvState* state;
  uint32_t guest_env;
  uint32_t wasm_fn_ptr;
  uint32_t data;
  uint32_t hint;
};

void GuestFinalizerTrampoline(node_api_basic_env /*env*/, void* /*data*/,
                              void* hint) {
  auto* record = static_cast<GuestFinalizerRecord*>(hint);
  if (record == nullptr) return;
  auto* bridge_state = LookupEnvState(record->state);
  GuestCallScope guest_call(bridge_state);
  void* callback_ctx =
      bridge_state != nullptr && guest_call.allowed()
          ? bridge_state->agent->active_callback_ctx.load(std::memory_order_acquire)
          : nullptr;
  if (callback_ctx != nullptr) {
    // Own any value ids the guest mints while running its finalizer; there is
    // no return value to escape, so a plain handle scope suffices (mirrors
    // WasmInterruptCallback).
    napi_env env = bridge_state->env;
    std::vector<ScopeFrame>& frames = bridge_state->agent->scope_frames;
    napi_handle_scope fin_scope = nullptr;
    if (env != nullptr &&
        napi_open_handle_scope(env, &fin_scope) == napi_ok &&
        fin_scope != nullptr) {
      PushScopeFrame(*bridge_state, fin_scope, nullptr);
    }
    snapi_host_invoke_wasm_finalizer(callback_ctx, record->guest_env,
                                     record->wasm_fn_ptr, record->data,
                                     record->hint);
    if (fin_scope != nullptr) {
      while (frames.size() > 1 && frames.back().scope != fin_scope) {
        PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
      }
      if (!frames.empty() && frames.back().scope == fin_scope) {
        PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
      }
    }
  }
  delete record;
}

GuestFinalizerRecord* MakeGuestFinalizerRecord(SnapiEnvState* state,
                                               uint32_t guest_env,
                                               uint32_t wasm_fn_ptr,
                                               uint32_t data, uint32_t hint) {
  auto* record = new (std::nothrow) GuestFinalizerRecord();
  if (record != nullptr) {
    record->state = state;
    record->guest_env = guest_env;
    record->wasm_fn_ptr = wasm_fn_ptr;
    record->data = data;
    record->hint = hint;
  }
  return record;
}
}  // namespace

// Like snapi_bridge_create_external_arraybuffer, but re-runs the guest's own
// finalize callback (a wasm function pointer) when V8 collects the value, so
// guest-owned external allocations are reclaimed instead of leaked.
extern "C" int snapi_bridge_create_external_arraybuffer_guest_finalized(
    SnapiEnvState* env_state, uint64_t data_addr, uint32_t byte_length,
    uint32_t guest_env, uint32_t wasm_fn_ptr, uint32_t finalize_data,
    uint32_t finalize_hint, uint64_t* backing_store_token_out,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_arraybuffer(
      env, data, (size_t)byte_length, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  auto* record = MakeGuestFinalizerRecord(env_state, guest_env, wasm_fn_ptr,
                                          finalize_data, finalize_hint);
  if (record == nullptr) return napi_generic_failure;
  s = napi_add_finalizer(env, result, nullptr, GuestFinalizerTrampoline, record,
                         nullptr);
  if (s != napi_ok) {
    delete record;
    return s;
  }
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// Buffer twin of snapi_bridge_create_external_arraybuffer_guest_finalized.
extern "C" int snapi_bridge_create_external_buffer_guest_finalized(
    SnapiEnvState* env_state, uint64_t data_addr, uint32_t byte_length,
    uint32_t guest_env, uint32_t wasm_fn_ptr, uint32_t finalize_data,
    uint32_t finalize_hint, uint64_t* backing_store_token_out,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_buffer(
      env, (size_t)byte_length, data, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  auto* record = MakeGuestFinalizerRecord(env_state, guest_env, wasm_fn_ptr,
                                          finalize_data, finalize_hint);
  if (record == nullptr) return napi_generic_failure;
  s = napi_add_finalizer(env, result, nullptr, GuestFinalizerTrampoline, record,
                         nullptr);
  if (s != napi_ok) {
    delete record;
    return s;
  }
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// Like snapi_bridge_create_external_arraybuffer, but the buffer's lifetime is
// tied to the JS value: when V8 collects it, `finalize_hint` is handed to the
// Rust guest-heap finalizer, which frees the guest allocation. Ownership of
// `finalize_hint` transfers only on success.
extern "C" int snapi_bridge_create_external_arraybuffer_finalized(
    SnapiEnvState* env_state, uint64_t data_addr, uint32_t byte_length,
    void* finalize_hint, uint64_t* backing_store_token_out, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_arraybuffer(
      env, data, (size_t)byte_length, GuestHeapBufferFinalizeTrampoline,
      finalize_hint, &result);
  if (s != napi_ok) return s;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// Buffer twin of snapi_bridge_create_external_arraybuffer_finalized.
extern "C" int snapi_bridge_create_external_buffer_finalized(
    SnapiEnvState* env_state, uint64_t data_addr, uint32_t byte_length,
    void* finalize_hint, uint64_t* backing_store_token_out, uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_buffer(
      env, (size_t)byte_length, data, GuestHeapBufferFinalizeTrampoline,
      finalize_hint, &result);
  if (s != napi_ok) return s;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_external_arraybuffer(SnapiEnvState* env_state, uint64_t data_addr,
                                                        uint32_t byte_length,
                                                        uint64_t* backing_store_token_out,
                                                        uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_arraybuffer(
      env, data, (size_t)byte_length, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_external_buffer(SnapiEnvState* env_state, uint64_t data_addr,
                                                   uint32_t byte_length,
                                                   uint64_t* backing_store_token_out,
                                                   uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_buffer(
      env, (size_t)byte_length, data, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, result);
  }
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_is_sharedarraybuffer(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is_sab = false;
  napi_status s = node_api_is_sharedarraybuffer(env, val, &is_sab);
  if (s != napi_ok) return s;
  *result = is_sab ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_create_sharedarraybuffer(SnapiEnvState* env_state, uint32_t byte_length,
                                                     uint64_t* data_out,
                                                     uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = nullptr;
  napi_value result;
  napi_status s =
      node_api_create_sharedarraybuffer(env, (size_t)byte_length, &data, &result);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_node_api_set_prototype(SnapiEnvState* env_state, uint32_t object_id,
                                                   uint32_t prototype_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value object = LoadValue(*bridge_state, object_id);
  napi_value prototype = LoadValue(*bridge_state, prototype_id);
  if (!object || !prototype) return napi_invalid_arg;
  return node_api_set_prototype(env, object, prototype);
}

extern "C" int snapi_bridge_get_arraybuffer_info(SnapiEnvState* env_state, uint32_t id,
                                                 uint64_t* data_out,
                                                 uint32_t* byte_length,
                                                 uint64_t* backing_store_token_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  void* data;
  size_t len;
  napi_status s = napi_get_arraybuffer_info(env, val, &data, &len);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  *byte_length = (uint32_t)len;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_backing_store_token(env, val);
  }
  return napi_ok;
}

extern "C" int snapi_bridge_detach_arraybuffer(SnapiEnvState* env_state, uint32_t id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_detach_arraybuffer(env, val);
}

extern "C" int snapi_bridge_is_detached_arraybuffer(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_detached_arraybuffer(env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

// ============================================================
// TypedArray
// ============================================================

extern "C" int snapi_bridge_create_typedarray(SnapiEnvState* env_state, int type, uint32_t length,
                                              uint32_t arraybuffer_id,
                                              uint32_t byte_offset,
                                              uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value arraybuffer = LoadValue(*bridge_state, arraybuffer_id);
  if (!arraybuffer) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_typedarray(
      env, (napi_typedarray_type)type, (size_t)length, arraybuffer,
      (size_t)byte_offset, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_typedarray_info(SnapiEnvState* env_state, uint32_t id, int* type_out,
                                                uint32_t* length_out,
                                                uint64_t* data_out,
                                                uint32_t* arraybuffer_out,
                                                uint32_t* byte_offset_out,
                                                uint64_t* backing_store_token_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  napi_typedarray_type type;
  size_t length;
  void* data = nullptr;
  napi_value arraybuffer;
  size_t byte_offset;
  napi_status s = napi_get_typedarray_info(env, val, &type, &length, &data,
                                           &arraybuffer, &byte_offset);
  if (s != napi_ok) return s;
  if (type_out) *type_out = (int)type;
  if (length_out) *length_out = (uint32_t)length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  if (arraybuffer_out) *arraybuffer_out = StoreValue(*bridge_state, arraybuffer);
  if (byte_offset_out) *byte_offset_out = (uint32_t)byte_offset;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, val);
  }
  return napi_ok;
}

// ============================================================
// DataView
// ============================================================

extern "C" int snapi_bridge_create_dataview(SnapiEnvState* env_state, uint32_t byte_length,
                                            uint32_t arraybuffer_id,
                                            uint32_t byte_offset,
                                            uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value arraybuffer = LoadValue(*bridge_state, arraybuffer_id);
  if (!arraybuffer) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_dataview(env, (size_t)byte_length, arraybuffer,
                                       (size_t)byte_offset, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_dataview_info(SnapiEnvState* env_state, uint32_t id,
                                              uint32_t* byte_length_out,
                                              uint64_t* data_out,
                                              uint32_t* arraybuffer_out,
                                              uint32_t* byte_offset_out,
                                              uint64_t* backing_store_token_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  size_t byte_length;
  void* data = nullptr;
  napi_value arraybuffer;
  size_t byte_offset;
  napi_status s = napi_get_dataview_info(env, val, &byte_length, &data,
                                         &arraybuffer, &byte_offset);
  if (s != napi_ok) return s;
  if (byte_length_out) *byte_length_out = (uint32_t)byte_length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  if (arraybuffer_out) *arraybuffer_out = StoreValue(*bridge_state, arraybuffer);
  if (byte_offset_out) *byte_offset_out = (uint32_t)byte_offset;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, val);
  }
  return napi_ok;
}

// Attach the guest-heap finalizer to an arbitrary value: `finalize_hint` is
// handed to the Rust guest-heap finalizer when V8 collects the value. Used to
// tie the lifetime of a snapshot copy (foreign backing stores) to its value.
extern "C" int snapi_bridge_attach_guest_heap_finalizer(SnapiEnvState* env_state,
                                                        uint32_t id,
                                                        void* finalize_hint) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_add_finalizer(bridge_state->env, val, nullptr,
                            GuestHeapBufferFinalizeTrampoline, finalize_hint,
                            nullptr);
}

// ============================================================
// External values
// ============================================================

extern "C" int snapi_bridge_create_external(SnapiEnvState* env_state, uint64_t data_val,
                                            uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  // Store arbitrary u64 data value as a void*. No finalizer.
  napi_value result;
  napi_status s = napi_create_external(env, (void*)(uintptr_t)data_val,
                                       nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_external(SnapiEnvState* env_state, uint32_t id,
                                               uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  void* data;
  napi_status s = napi_get_value_external(env, val, &data);
  if (s != napi_ok) return s;
  *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

// ============================================================
// References
// ============================================================

extern "C" int snapi_bridge_create_reference(SnapiEnvState* env_state, uint32_t value_id,
                                             uint32_t initial_refcount,
                                             uint32_t* ref_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, value_id);
  if (!val) return napi_invalid_arg;
  napi_ref ref;
  napi_status s = napi_create_reference(env, val, initial_refcount, &ref);
  if (s != napi_ok) return s;
  *ref_out = StoreRef(*bridge_state, ref);
  return napi_ok;
}

extern "C" int snapi_bridge_delete_reference(SnapiEnvState* env_state, uint32_t ref_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_ref ref = LoadRef(*bridge_state, ref_id);
  if (!ref) return napi_invalid_arg;
  napi_status s = napi_delete_reference(env, ref);
  if (s == napi_ok) RemoveRef(*bridge_state, ref_id);
  return s;
}

extern "C" int snapi_bridge_reference_ref(SnapiEnvState* env_state, uint32_t ref_id, uint32_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_ref ref = LoadRef(*bridge_state, ref_id);
  if (!ref) return napi_invalid_arg;
  return napi_reference_ref(env, ref, result);
}

extern "C" int snapi_bridge_reference_unref(SnapiEnvState* env_state, uint32_t ref_id, uint32_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_ref ref = LoadRef(*bridge_state, ref_id);
  if (!ref) return napi_invalid_arg;
  return napi_reference_unref(env, ref, result);
}

extern "C" int snapi_bridge_get_reference_value(SnapiEnvState* env_state, uint32_t ref_id,
                                                uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_ref ref = LoadRef(*bridge_state, ref_id);
  if (!ref) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_reference_value(env, ref, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Handle scopes
// ============================================================

extern "C" int snapi_bridge_open_handle_scope(SnapiEnvState* env_state, uint32_t* scope_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_handle_scope scope = nullptr;
  napi_status s = napi_open_handle_scope(env, &scope);
  if (s != napi_ok) return s;
  SnapiAgentState& agent = *bridge_state->agent;
  ScopeFrame& frame = PushScopeFrame(*bridge_state, scope, nullptr);
  frame.id = agent.next_scope_id++;
  if (frame.id == 0) frame.id = agent.next_scope_id++;
  *scope_out = frame.id;
  return napi_ok;
}

extern "C" int snapi_bridge_close_handle_scope(SnapiEnvState* env_state, uint32_t scope_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  const int idx = FindFrameById(*bridge_state, scope_id);
  const std::vector<ScopeFrame>& frames = bridge_state->agent->scope_frames;
  if (idx < 0 || frames[idx].scope == nullptr) {
    return napi_invalid_arg;
  }
  // LIFO discipline: only the innermost frame may close. Implicit callback
  // frames have id 0 and can never match, so a guest cannot close past them.
  if (static_cast<size_t>(idx) != frames.size() - 1) {
    return napi_handle_scope_mismatch;
  }
  PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
  return napi_ok;
}

extern "C" int snapi_bridge_open_escapable_handle_scope(SnapiEnvState* env_state, uint32_t* scope_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_escapable_handle_scope scope = nullptr;
  napi_status s = napi_open_escapable_handle_scope(env, &scope);
  if (s != napi_ok) return s;
  SnapiAgentState& agent = *bridge_state->agent;
  ScopeFrame& frame = PushScopeFrame(*bridge_state, nullptr, scope);
  frame.id = agent.next_scope_id++;
  if (frame.id == 0) frame.id = agent.next_scope_id++;
  *scope_out = frame.id;
  return napi_ok;
}

extern "C" int snapi_bridge_close_escapable_handle_scope(SnapiEnvState* env_state, uint32_t scope_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  const int idx = FindFrameById(*bridge_state, scope_id);
  const std::vector<ScopeFrame>& frames = bridge_state->agent->scope_frames;
  if (idx < 0 || frames[idx].esc_scope == nullptr) {
    return napi_invalid_arg;
  }
  if (static_cast<size_t>(idx) != frames.size() - 1) {
    return napi_handle_scope_mismatch;
  }
  PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
  return napi_ok;
}

extern "C" int snapi_bridge_escape_handle(SnapiEnvState* env_state, uint32_t scope_id,
                                          uint32_t escapee_id,
                                          uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  const int idx = FindFrameById(*bridge_state, scope_id);
  std::vector<ScopeFrame>& frames = bridge_state->agent->scope_frames;
  // The escaped Local is created in the scope enclosing the escapable one, so
  // its id must be owned by the parent frame; require one to exist.
  if (idx <= 0 || frames[idx].esc_scope == nullptr) {
    return napi_invalid_arg;
  }
  napi_value escapee = LoadValue(*bridge_state, escapee_id);
  if (!escapee) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s =
      napi_escape_handle(env, frames[idx].esc_scope, escapee, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValueInFrame(*bridge_state, result, frames[idx - 1]);
  return napi_ok;
}

// ============================================================
// Type tagging
// ============================================================

extern "C" int snapi_bridge_type_tag_object(SnapiEnvState* env_state, uint32_t obj_id,
                                            uint64_t tag_lower,
                                            uint64_t tag_upper) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_type_tag tag;
  tag.lower = tag_lower;
  tag.upper = tag_upper;
  return napi_type_tag_object(env, obj, &tag);
}

extern "C" int snapi_bridge_check_object_type_tag(SnapiEnvState* env_state, uint32_t obj_id,
                                                  uint64_t tag_lower,
                                                  uint64_t tag_upper,
                                                  int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_type_tag tag;
  tag.lower = tag_lower;
  tag.upper = tag_upper;
  bool matches;
  napi_status s = napi_check_object_type_tag(env, obj, &tag, &matches);
  if (s != napi_ok) return s;
  *result = matches ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Function calling (call JS functions from native)
// ============================================================

extern "C" int snapi_bridge_call_function(SnapiEnvState* env_state, uint32_t recv_id, uint32_t func_id,
                                          uint32_t argc,
                                          const uint32_t* argv_ids,
                                          uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value recv = LoadValue(*bridge_state, recv_id);
  napi_value func = LoadValue(*bridge_state, func_id);
  if (!recv || !func) return napi_invalid_arg;
  std::vector<napi_value> argv(argc);
  for (uint32_t i = 0; i < argc; i++) {
    argv[i] = LoadValue(*bridge_state, argv_ids[i]);
    if (!argv[i]) return napi_invalid_arg;
  }
  napi_value result;
  napi_status s =
      napi_call_function(env, recv, func, argc, argv.data(), &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Script execution
// ============================================================

extern "C" int snapi_bridge_run_script(SnapiEnvState* env_state, uint32_t script_id,
                                       uint32_t* out_value_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value script_val = LoadValue(*bridge_state, script_id);
  if (!script_val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_run_script(env, script_val, &result);
  if (s != napi_ok) return s;
  *out_value_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// UTF-16 strings
// ============================================================

extern "C" int snapi_bridge_create_string_utf16(SnapiEnvState* env_state, const uint16_t* str,
                                                uint32_t wasm_length,
                                                uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_utf16(env, (const char16_t*)str, length,
                                           &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_string_utf16(SnapiEnvState* env_state, uint32_t id,
                                                   uint16_t* buf,
                                                   size_t bufsize,
                                                   size_t* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_utf16(env, val, (char16_t*)buf, bufsize,
                                     result);
}

// ============================================================
// BigInt words (arbitrary precision)
// ============================================================

extern "C" int snapi_bridge_create_bigint_words(SnapiEnvState* env_state, int sign_bit,
                                                uint32_t word_count,
                                                const uint64_t* words,
                                                uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result;
  napi_status s = napi_create_bigint_words(env, sign_bit, (size_t)word_count,
                                           words, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_words(SnapiEnvState* env_state, uint32_t id,
                                                   int* sign_bit,
                                                   size_t* word_count,
                                                   uint64_t* words) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_bigint_words(env, val, sign_bit, word_count, words);
}

// ============================================================
// Instance data
// ============================================================

extern "C" int snapi_bridge_set_instance_data(SnapiEnvState* env_state, uint64_t data_val) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return napi_set_instance_data(env, (void*)(uintptr_t)data_val,
                                nullptr, nullptr);
}

extern "C" int snapi_bridge_get_instance_data(SnapiEnvState* env_state, uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  void* data = nullptr;
  napi_status s = napi_get_instance_data(env, &data);
  if (s != napi_ok) return s;
  *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_adjust_external_memory(SnapiEnvState* env_state, int64_t change,
                                                   int64_t* adjusted) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return napi_adjust_external_memory(env, change, adjusted);
}

// ============================================================
// Node Buffers
// ============================================================

extern "C" int snapi_bridge_create_buffer(SnapiEnvState* env_state, uint32_t length,
                                          uint64_t* data_out,
                                          uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value buffer;
  void* data = nullptr;
  napi_status s = napi_create_buffer(env, (size_t)length, &data, &buffer);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, buffer);
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_create_buffer_copy(SnapiEnvState* env_state, uint32_t length,
                                               const void* src_data,
                                               uint64_t* result_data_out,
                                               uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value buffer;
  void* result_data = nullptr;
  napi_status s = napi_create_buffer_copy(env, (size_t)length, src_data,
                                          &result_data, &buffer);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, buffer);
  if (result_data_out) *result_data_out = (uint64_t)(uintptr_t)result_data;
  return napi_ok;
}

extern "C" int snapi_bridge_is_buffer(SnapiEnvState* env_state, uint32_t id, int* result) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  bool is_buffer = false;
  napi_status s = napi_is_buffer(env, val, &is_buffer);
  if (s != napi_ok) return s;
  *result = is_buffer ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_buffer_info(SnapiEnvState* env_state, uint32_t id,
                                            uint64_t* data_out,
                                            uint32_t* length_out,
                                            uint64_t* backing_store_token_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value val = LoadValue(*bridge_state, id);
  if (!val) return napi_invalid_arg;
  void* data = nullptr;
  size_t length = 0;
  napi_status s = napi_get_buffer_info(env, val, &data, &length);
  if (s != napi_ok) return s;
  if (length_out) *length_out = (uint32_t)length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  if (backing_store_token_out) {
    *backing_store_token_out = napi_v8_get_arraybuffer_view_backing_store_token(env, val);
  }
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_acquire_buffer_lease(
    SnapiEnvState* env_state, uint32_t value_id, uint32_t byte_offset,
    uint32_t byte_length, int mode, uint32_t* lease_out, uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || lease_out == nullptr || data_out == nullptr) {
    return napi_invalid_arg;
  }
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  unofficial_napi_buffer_lease lease = nullptr;
  void* data = nullptr;
  napi_status status = unofficial_napi_acquire_buffer_lease(
      bridge_state->env, value, byte_offset, byte_length,
      static_cast<unofficial_napi_buffer_access_mode>(mode), &lease, &data);
  if (status != napi_ok) return status;
  const uint32_t lease_id = bridge_state->agent->buffer_lease_handles.Store(lease);
  if (lease_id == 0) {
    (void)unofficial_napi_release_buffer_lease(bridge_state->env, lease, false);
    return napi_generic_failure;
  }
  *lease_out = lease_id;
  *data_out = reinterpret_cast<uint64_t>(data);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_release_buffer_lease(
    SnapiEnvState* env_state, uint32_t lease_id, int modified) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  auto lease = static_cast<unofficial_napi_buffer_lease>(
      bridge_state->agent->buffer_lease_handles.Load(lease_id));
  if (lease == nullptr) return napi_invalid_arg;
  napi_status status = unofficial_napi_release_buffer_lease(
      bridge_state->env, lease, modified != 0);
  bridge_state->agent->buffer_lease_handles.Remove(lease_id);
  return status;
}

extern "C" int snapi_bridge_unofficial_create_guest_backed_typedarray(
    SnapiEnvState* env_state, int type, uint32_t length, uint64_t* data_out,
    uint32_t* value_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || data_out == nullptr || value_out == nullptr) {
    return napi_invalid_arg;
  }
  void* data = nullptr;
  napi_value value = nullptr;
  napi_status status = unofficial_napi_create_guest_backed_typedarray(
      bridge_state->env, static_cast<napi_typedarray_type>(type), length, &data,
      &value);
  if (status != napi_ok) return status;
  *data_out = reinterpret_cast<uint64_t>(data);
  *value_out = StoreValue(*bridge_state, value);
  return *value_out == 0 ? napi_generic_failure : napi_ok;
}

// ============================================================
// Node version (stub — we're not running in Node, return fake version)
// ============================================================

extern "C" int snapi_bridge_get_node_version(SnapiEnvState* env_state, uint32_t* major,
                                             uint32_t* minor,
                                             uint32_t* patch) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  // Return a reasonable fake version since we're running on pure V8
  if (major) *major = 22;
  if (minor) *minor = 0;
  if (patch) *patch = 0;
  return napi_ok;
}

// ============================================================
// Object wrapping
// ============================================================

extern "C" int snapi_bridge_wrap(SnapiEnvState* env_state, uint32_t obj_id, uint64_t native_data,
                                 uint32_t* ref_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  napi_ref ref = nullptr;
  napi_status s = napi_wrap(env, obj, (void*)(uintptr_t)native_data,
                            nullptr, nullptr, ref_out ? &ref : nullptr);
  if (s != napi_ok) return s;
  if (ref_out) *ref_out = StoreRef(*bridge_state, ref);
  return napi_ok;
}

// Like snapi_bridge_wrap, but runs the guest's finalize callback when the
// wrapped object is collected (napi_wrap's finalizer goes through the deferred
// RefBase drain, so the guest can be re-entered safely).
extern "C" int snapi_bridge_wrap_finalized(SnapiEnvState* env_state, uint32_t obj_id,
                                           uint64_t native_data, uint32_t guest_env,
                                           uint32_t wasm_fn_ptr, uint32_t finalize_data,
                                           uint32_t finalize_hint, uint32_t* ref_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  auto* record = MakeGuestFinalizerRecord(env_state, guest_env, wasm_fn_ptr,
                                          finalize_data, finalize_hint);
  if (record == nullptr) return napi_generic_failure;
  napi_ref ref = nullptr;
  napi_status s = napi_wrap(env, obj, (void*)(uintptr_t)native_data,
                            GuestFinalizerTrampoline, record,
                            ref_out ? &ref : nullptr);
  if (s != napi_ok) {
    delete record;
    return s;
  }
  if (ref_out) *ref_out = StoreRef(*bridge_state, ref);
  return napi_ok;
}

extern "C" int snapi_bridge_unwrap(SnapiEnvState* env_state, uint32_t obj_id, uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  void* data = nullptr;
  napi_status s = napi_unwrap(env, obj, &data);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_remove_wrap(SnapiEnvState* env_state, uint32_t obj_id, uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  void* data = nullptr;
  napi_status s = napi_remove_wrap(env, obj, &data);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_add_finalizer(SnapiEnvState* env_state, uint32_t obj_id, uint64_t data_val,
                                          uint32_t* ref_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  // No actual WASM callback for finalizer; just register with nullptr callback
  napi_ref ref = nullptr;
  napi_status s = napi_add_finalizer(env, obj, (void*)(uintptr_t)data_val,
                                     nullptr, nullptr, ref_out ? &ref : nullptr);
  if (s != napi_ok) return s;
  if (ref_out) *ref_out = StoreRef(*bridge_state, ref);
  return napi_ok;
}

// Like snapi_bridge_add_finalizer, but runs the guest's finalize callback when
// the object is collected (goes through the deferred RefBase drain).
extern "C" int snapi_bridge_add_finalizer_cb(SnapiEnvState* env_state, uint32_t obj_id,
                                             uint64_t data_val, uint32_t guest_env,
                                             uint32_t wasm_fn_ptr, uint32_t finalize_data,
                                             uint32_t finalize_hint, uint32_t* ref_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  auto* record = MakeGuestFinalizerRecord(env_state, guest_env, wasm_fn_ptr,
                                          finalize_data, finalize_hint);
  if (record == nullptr) return napi_generic_failure;
  napi_ref ref = nullptr;
  napi_status s = napi_add_finalizer(env, obj, (void*)(uintptr_t)data_val,
                                     GuestFinalizerTrampoline, record,
                                     ref_out ? &ref : nullptr);
  if (s != napi_ok) {
    delete record;
    return s;
  }
  if (ref_out) *ref_out = StoreRef(*bridge_state, ref);
  return napi_ok;
}

// ============================================================
// napi_new_instance
// ============================================================

extern "C" int snapi_bridge_new_instance(SnapiEnvState* env_state, uint32_t ctor_id, uint32_t argc,
                                         const uint32_t* argv_ids,
                                         uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value ctor = LoadValue(*bridge_state, ctor_id);
  if (!ctor) return napi_invalid_arg;
  std::vector<napi_value> argv(argc);
  for (uint32_t i = 0; i < argc; i++) {
    argv[i] = LoadValue(*bridge_state, argv_ids[i]);
    if (!argv[i]) return napi_invalid_arg;
  }
  napi_value result;
  napi_status s = napi_new_instance(env, ctor, argc, argv.data(), &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// napi_define_properties
// ============================================================

// Forward declaration (defined below in callback system section)
static napi_value generic_wasm_callback(napi_env env, napi_callback_info info);

extern "C" int snapi_bridge_define_properties(SnapiEnvState* env_state, uint32_t obj_id,
                                              uint32_t prop_count,
                                              const char** utf8names,
                                              const uint32_t* name_ids,
                                              const uint32_t* prop_types,
                                              const uint32_t* value_ids,
                                              const uint32_t* method_reg_ids,
                                              const uint32_t* getter_reg_ids,
                                              const uint32_t* setter_reg_ids,
                                              const int32_t* attributes) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value obj = LoadValue(*bridge_state, obj_id);
  if (!obj) return napi_invalid_arg;
  std::vector<napi_property_descriptor> descs(prop_count);
  for (uint32_t i = 0; i < prop_count; i++) {
    memset(&descs[i], 0, sizeof(napi_property_descriptor));
    descs[i].utf8name = utf8names != nullptr ? utf8names[i] : nullptr;
    descs[i].name = (name_ids != nullptr && name_ids[i] != 0) ? LoadValue(*bridge_state, name_ids[i]) : nullptr;
    descs[i].attributes = (napi_property_attributes)attributes[i];

    switch (prop_types[i]) {
      case 0:
        descs[i].value = LoadValue(*bridge_state, value_ids[i]);
        break;
      case 1:
        descs[i].method = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, method_reg_ids[i]);
        break;
      case 2:
        descs[i].getter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, getter_reg_ids[i]);
        break;
      case 3:
        descs[i].setter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, setter_reg_ids[i]);
        break;
      case 4:
        descs[i].getter = generic_wasm_callback;
        descs[i].setter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, getter_reg_ids[i]);
        break;
    }
  }
  return napi_define_properties(env, obj, prop_count, descs.data());
}

// ============================================================
// napi_define_class
// ============================================================

// Property descriptor layout passed from Rust:
// For each property (i), we pass:
//   utf8names[i]   - property name (C string)
//   types[i]       - 0=value, 1=method, 2=getter, 3=setter, 4=getter+setter
//   value_ids[i]   - if type==0, the value handle ID
//   method_reg_ids[i]  - if type==1, the callback reg_id for the method
//   getter_reg_ids[i]  - if type==2 or 4, the callback reg_id for getter
//   setter_reg_ids[i]  - if type==3 or 4, the callback reg_id for setter
//   attributes[i]  - napi_property_attributes

extern "C" int snapi_bridge_define_class(SnapiEnvState* env_state, 
    const char* utf8name, uint32_t name_len,
    uint32_t ctor_reg_id,
    uint32_t prop_count,
    const char** prop_names,
    const uint32_t* prop_name_ids,
    const uint32_t* prop_types,
    const uint32_t* prop_value_ids,
    const uint32_t* prop_method_reg_ids,
    const uint32_t* prop_getter_reg_ids,
    const uint32_t* prop_setter_reg_ids,
    const int32_t* prop_attributes,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;

  // Build property descriptors
  std::vector<napi_property_descriptor> descs(prop_count);
  for (uint32_t i = 0; i < prop_count; i++) {
    memset(&descs[i], 0, sizeof(napi_property_descriptor));
    descs[i].utf8name = prop_names != nullptr ? prop_names[i] : nullptr;
    descs[i].name = (prop_name_ids != nullptr && prop_name_ids[i] != 0) ? LoadValue(*bridge_state, prop_name_ids[i]) : nullptr;
    descs[i].attributes = (napi_property_attributes)prop_attributes[i];

    switch (prop_types[i]) {
      case 0: // value
        descs[i].value = LoadValue(*bridge_state, prop_value_ids[i]);
        break;
      case 1: // method
        descs[i].method = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, prop_method_reg_ids[i]);
        break;
      case 2: // getter only
        descs[i].getter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, prop_getter_reg_ids[i]);
        break;
      case 3: // setter only
        descs[i].setter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, prop_setter_reg_ids[i]);
        break;
      case 4: // getter + setter
        descs[i].getter = generic_wasm_callback;
        descs[i].setter = generic_wasm_callback;
        descs[i].data = RegisterCallbackBinding(bridge_state, prop_getter_reg_ids[i]);
        // Note: N-API uses the same data pointer for both getter and setter.
        // The setter_reg_id is stored in the getter_reg_id for now.
        break;
    }
  }

  napi_value result;
  napi_status s = napi_define_class(
      env, utf8name,
      name_len == 0xFFFFFFFFu ? NAPI_AUTO_LENGTH : (size_t)name_len,
      generic_wasm_callback,
      RegisterCallbackBinding(bridge_state, ctor_reg_id),
      prop_count, descs.data(),
      &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Callback system (napi_create_function + napi_get_cb_info)
// ============================================================

extern "C" void* snapi_bridge_swap_active_callback_ctx(SnapiEnvState* env_state, void* callback_ctx) {
  auto* bridge_state = LookupEnvState(env_state);
  if (bridge_state == nullptr) return nullptr;
  return bridge_state->agent->active_callback_ctx.exchange(callback_ctx,
                                                           std::memory_order_acq_rel);
}

extern "C" int snapi_bridge_get_cb_info(SnapiEnvState* env_state, uint32_t cbinfo_id,
                                        uint32_t* argc_ptr, uint32_t* argv_out,
                                        uint32_t max_argv,
                                        uint32_t* this_out, uint64_t* data_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_callback_info info = FindCallbackInvocation(*bridge_state, cbinfo_id);
  if (info == nullptr) return napi_generic_failure;

  size_t wanted = argc_ptr != nullptr ? static_cast<size_t>(*argc_ptr) : 0;
  size_t actual = wanted;
  std::vector<napi_value> argv(wanted);
  napi_value this_arg = nullptr;
  void* raw_data = nullptr;
  napi_status s =
      napi_get_cb_info(env, info, &actual, wanted > 0 ? argv.data() : nullptr,
                       &this_arg, &raw_data);
  if (s != napi_ok) return s;

  if (argc_ptr) *argc_ptr = static_cast<uint32_t>(actual);
  if (this_out) *this_out = this_arg != nullptr ? StoreValue(*bridge_state, this_arg) : 0;

  auto* binding = static_cast<CallbackBinding*>(raw_data);
  uint64_t data_val = 0;
  if (binding != nullptr) {
    auto reg_it = bridge_state->cb_registry.find(binding->reg_id);
    if (reg_it != bridge_state->cb_registry.end()) {
      data_val = reg_it->second.data_val;
    }
  }
  if (data_out) *data_out = data_val;

  if (argv_out != nullptr) {
    // Slots past the actual arguments hold undefined (the engine filled
    // them); one id serves them all.
    uint32_t undefined_id = 0;
    for (size_t i = 0; i < wanted; i++) {
      if (i < actual) {
        argv_out[i] = StoreValue(*bridge_state, argv[i]);
      } else {
        if (undefined_id == 0) undefined_id = StoreValue(*bridge_state, argv[i]);
        argv_out[i] = undefined_id;
      }
    }
  }
  return napi_ok;
}

// napi_get_new_target — only valid inside a constructor callback
extern "C" int snapi_bridge_get_new_target(SnapiEnvState* env_state, uint32_t cbinfo_id,
                                           uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_callback_info info = FindCallbackInvocation(*bridge_state, cbinfo_id);
  if (info == nullptr) return napi_generic_failure;
  napi_value result;
  napi_status s = napi_get_new_target(env, info, &result);
  if (s != napi_ok) return s;
  *out_id = result ? StoreValue(*bridge_state, result) : 0;
  return napi_ok;
}

// Forward-declare the Rust trampoline (defined in lib.rs via #[no_mangle] extern "C")
extern "C" uint32_t snapi_host_invoke_wasm_callback(void* callback_ctx,
                                                    uint32_t guest_env,
                                                    uint32_t wasm_fn_ptr,
                                                    uint32_t callback_arg);

void WasmInterruptCallback(napi_env env, void* raw) {
  auto* request = static_cast<WasmInterruptRequest*>(raw);
  if (request == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    request->state->agent->pending_interrupts.erase(request);
  }
  auto* bridge_state = LookupEnvState(request->state);
  // An env whose agent is being released no longer runs guest interrupt
  // callbacks: its scopes and handle tables are being torn down. `closing` is
  // only written by the owner thread, which is the thread running this
  // callback.
  if (bridge_state != nullptr && bridge_state->agent->closing) bridge_state = nullptr;
  void* callback_ctx =
      bridge_state != nullptr
          ? bridge_state->agent->active_callback_ctx.load(std::memory_order_acquire)
          : nullptr;
  if (callback_ctx != nullptr) {
    // Own the ids the guest mints while servicing the interrupt (no return
    // value to escape, so a plain frame suffices).
    std::vector<ScopeFrame>& frames = bridge_state->agent->scope_frames;
    napi_handle_scope interrupt_scope = nullptr;
    if (env != nullptr &&
        napi_open_handle_scope(env, &interrupt_scope) == napi_ok &&
        interrupt_scope != nullptr) {
      PushScopeFrame(*bridge_state, interrupt_scope, nullptr);
    }
    snapi_host_invoke_wasm_callback(
        callback_ctx, request->guest_env, request->wasm_fn_ptr, request->callback_arg);
    if (interrupt_scope != nullptr) {
      while (frames.size() > 1 && frames.back().scope != interrupt_scope) {
        PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
      }
      if (!frames.empty() && frames.back().scope == interrupt_scope) {
        PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
      }
    }
  }
  delete request;
}

// Generic C++ callback invoked by V8 for all napi_create_function functions.
// Passes the real callback frame explicitly to the Rust trampoline via a
// short-lived callback-info token.
static napi_value generic_wasm_callback(napi_env env, napi_callback_info info) {
  void* raw_data = nullptr;
  // Only the actual argument count and registration data select the guest
  // callback. Guest napi_get_cb_info retrieves arguments/receiver on demand.
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, nullptr, &raw_data);

  auto* binding = static_cast<CallbackBinding*>(raw_data);
  if (binding == nullptr) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
  auto* bridge_state = LookupEnvState(binding->state);
  if (bridge_state == nullptr) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
  SnapiAgentState& agent = *bridge_state->agent;
  GuestCallScope guest_call(bridge_state);
  void* callback_ctx =
      guest_call.allowed() ? agent.active_callback_ctx.load(std::memory_order_acquire)
                           : nullptr;
  if (callback_ctx == nullptr) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
  auto it = bridge_state->cb_registry.find(binding->reg_id);
  if (it == bridge_state->cb_registry.end()) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
  const uint32_t cbinfo_id = RegisterCallbackInvocation(bridge_state, info);
  if (cbinfo_id == 0) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Implicit per-callback scope: ids minted while the guest services this
  // callback (arguments, intermediates, the return id) die when it returns,
  // matching N-API semantics. Escapable so the return value can be handed
  // back to V8's trampoline in the enclosing scope.
  napi_escapable_handle_scope cb_scope = nullptr;
  if (napi_open_escapable_handle_scope(env, &cb_scope) == napi_ok && cb_scope != nullptr) {
    PushScopeFrame(*bridge_state, nullptr, cb_scope);
  }

  // Call Rust trampoline → WASM callback
  const uint32_t wasm_fn_ptr =
      (it->second.wasm_setter_fn_ptr != 0 && argc > 0)
          ? it->second.wasm_setter_fn_ptr
          : it->second.wasm_fn_ptr;

  uint32_t result_id =
      snapi_host_invoke_wasm_callback(
          callback_ctx, it->second.guest_env, wasm_fn_ptr, cbinfo_id);
  // Callbacks nest, so ours is normally on top.
  auto& invocations = agent.callback_invocations;
  auto invocation = std::find_if(invocations.rbegin(), invocations.rend(),
                                 [&](const CallbackInvocation& i) { return i.id == cbinfo_id; });
  if (invocation != invocations.rend()) invocations.erase(std::next(invocation).base());

  napi_value result = LoadValue(*bridge_state, result_id);
  std::vector<ScopeFrame>& frames = agent.scope_frames;
  // Only while our frame is still open (it normally is the innermost one
  // the guest did not close).
  if (cb_scope != nullptr &&
      std::find_if(frames.rbegin(), frames.rend(), [&](const ScopeFrame& frame) {
        return frame.esc_scope == cb_scope;
      }) != frames.rend()) {
    // Escape first: the result may live in a scope the guest left open, which
    // closing would free. The escape slot belongs to the scope around ours.
    if (result != nullptr) {
      napi_value escaped = nullptr;
      result = napi_escape_handle(env, cb_scope, result, &escaped) == napi_ok
                   ? escaped
                   : nullptr;
    }
    // Then close the scopes the guest left open, and ours.
    while (frames.back().esc_scope != cb_scope) {
      PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
    }
    PopCurrentFrame(*bridge_state, /*close_napi_scope=*/true);
  }
  if (!result) {
    napi_get_undefined(env, &result);
  }
  return result;
}

// Allocate a registration ID for a new callback
extern "C" uint32_t snapi_bridge_alloc_cb_reg_id(SnapiEnvState* env_state) {
  env_state = RequireEnvState(env_state);
  if (env_state == nullptr) return 0;
  // Cap callback registrations alongside value handles: 0 signals failure so
  // the registration chain aborts instead of growing cb_registry unbounded.
  const SnapiAgentState& agent = *env_state->agent;
  if (agent.value_limit != 0 &&
      AgentCallbackRegistrations(agent) >= agent.value_limit) {
    return 0;
  }
  return env_state->next_cb_reg_id++;
}

// Register callback data for a registration ID
extern "C" void snapi_bridge_register_callback(SnapiEnvState* env_state,
                                               uint32_t reg_id,
                                               uint32_t guest_env,
                                               uint32_t wasm_fn_ptr,
                                               uint64_t data_val) {
  env_state = RequireEnvState(env_state);
  if (env_state == nullptr || reg_id == 0) return;
  env_state->cb_registry[reg_id] = { guest_env, wasm_fn_ptr, 0, data_val };
}

extern "C" void snapi_bridge_register_callback_pair(SnapiEnvState* env_state,
                                                    uint32_t reg_id,
                                                    uint32_t guest_env,
                                                    uint32_t wasm_getter_fn_ptr,
                                                    uint32_t wasm_setter_fn_ptr,
                                                    uint64_t data_val) {
  env_state = RequireEnvState(env_state);
  if (env_state == nullptr || reg_id == 0) return;
  env_state->cb_registry[reg_id] = { guest_env, wasm_getter_fn_ptr, wasm_setter_fn_ptr, data_val };
}

// Create a JS function with generic_wasm_callback as its native callback.
// The reg_id is passed as the data pointer so the callback can look up
// which WASM function to invoke.
extern "C" int snapi_bridge_create_function(SnapiEnvState* env_state, const char* utf8name, uint32_t name_len,
                                            uint32_t reg_id,
                                            uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  CallbackBinding* binding = RegisterCallbackBinding(bridge_state, reg_id);
  napi_value result;
  napi_status s = napi_create_function(env, utf8name,
                                       name_len == 0xFFFFFFFFu ? NAPI_AUTO_LENGTH : (size_t)name_len,
                                       generic_wasm_callback,
                                       binding,
                                       &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_create_env(int32_t module_api_version,
                                                  const void* guest_heap_ctx,
                                                  SnapiEnvState** env_out) {
  napi_env env = nullptr;
  unofficial_napi_env_owner owner = nullptr;
  napi_status s;
  if (guest_heap_ctx != nullptr) {
    unofficial_napi_env_create_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
    options.guest_heap = reinterpret_cast<unofficial_napi_guest_heap>(
        const_cast<void*>(guest_heap_ctx));
    s = unofficial_napi_create_env(module_api_version, &options, &env, &owner);
  } else {
    s = unofficial_napi_create_env(module_api_version, nullptr, &env, &owner);
  }
  if (s != napi_ok) return s;

  SnapiEnvState* state = NewAgentWithPrincipalEnv(env, owner);
  if (state == nullptr) return napi_generic_failure;

  if (env_out != nullptr) *env_out = state;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_configure_runtime(
    const char* engine_flags,
    uint32_t engine_flags_length) {
  unofficial_napi_runtime_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION;
  options.engine_flags = engine_flags;
  options.engine_flags_length = engine_flags_length;
  return unofficial_napi_configure_runtime(&options);
}

extern "C" int snapi_bridge_unofficial_create_env_with_options(
    int32_t module_api_version,
    uint64_t total_memory,
    uint64_t constrained_memory,
    uint32_t max_young_generation_size_in_bytes,
    uint32_t max_old_generation_size_in_bytes,
    uint32_t code_range_size_in_bytes,
    uint32_t /*stack_limit*/,
    const void* guest_heap_ctx,
    SnapiEnvState** env_out) {
  unofficial_napi_env_create_options options{};
  const bool has_options =
      max_young_generation_size_in_bytes > 0 ||
      max_old_generation_size_in_bytes > 0 ||
      code_range_size_in_bytes > 0 ||
      total_memory > 0 ||
      constrained_memory > 0 ||
      guest_heap_ctx != nullptr;
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  options.total_memory = total_memory;
  options.constrained_memory = constrained_memory;
  options.max_young_generation_size_in_bytes =
      max_young_generation_size_in_bytes;
  options.max_old_generation_size_in_bytes =
      max_old_generation_size_in_bytes;
  options.code_range_size_in_bytes = code_range_size_in_bytes;
  // The guest-provided stack limit is a Wasm linear-memory address, not a
  // native stack address for the host thread running V8.
  options.stack_limit = nullptr;
  options.guest_heap = reinterpret_cast<unofficial_napi_guest_heap>(
      const_cast<void*>(guest_heap_ctx));
  napi_env env = nullptr;
  unofficial_napi_env_owner owner = nullptr;
  napi_status s = unofficial_napi_create_env(
      module_api_version, has_options ? &options : nullptr, &env, &owner);
  if (s != napi_ok) return s;

  SnapiEnvState* state = NewAgentWithPrincipalEnv(env, owner);
  if (state == nullptr) return napi_generic_failure;

  if (env_out != nullptr) *env_out = state;
  return napi_ok;
}

// Rust-exported grant callback (see budget.rs). Charges a grow-step against
// the resource budget and returns the raised (or unchanged) heap limit.
extern "C" size_t napi_host_near_heap_limit_grant(const void* data,
                                                  size_t current_limit,
                                                  size_t initial_limit);

namespace {
// V8-shaped trampoline forwarding to the Rust grant callback. The budget
// tracker rides in `data`.
size_t HostNearHeapLimitTrampoline(napi_env /*env*/, void* data,
                                   size_t current_limit,
                                   size_t initial_limit) {
  return napi_host_near_heap_limit_grant(data, current_limit, initial_limit);
}
}  // namespace

extern "C" int snapi_bridge_unofficial_set_host_near_heap_limit_callback(
    SnapiEnvState* env_state, const void* data) {
  auto* state = RequireEnvState(env_state);
  if (state == nullptr) return napi_invalid_arg;
  return unofficial_napi_configure_near_heap_limit_callback(
      state->env,
      HostNearHeapLimitTrampoline,
      const_cast<void*>(data),
      0);
}

// Debug/diagnostics: live count of slot-table entries (leak assertions).
extern "C" uint64_t snapi_bridge_unofficial_get_live_value_count(SnapiEnvState* env_state) {
  auto* bridge_state = LookupEnvState(env_state);
  return bridge_state != nullptr
             ? static_cast<uint64_t>(bridge_state->agent->live_value_count)
             : 0;
}

// True if `id` still resolves. Used by the Rust layer to prune stale
// guest-memory data-pointer mappings keyed by value id.
extern "C" int snapi_bridge_value_id_alive(SnapiEnvState* env_state, uint32_t id) {
  auto* bridge_state = LookupEnvState(env_state);
  if (bridge_state == nullptr) return 0;
  return LoadValue(*bridge_state, id) != nullptr ? 1 : 0;
}

extern "C" int snapi_bridge_unofficial_set_value_limit(SnapiEnvState* env_state,
                                                       uint64_t limit) {
  auto* state = RequireEnvState(env_state);
  if (state == nullptr) return napi_invalid_arg;
  state->agent->value_limit = static_cast<size_t>(limit);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_release_env(SnapiEnvState* env_state) {
  return DisposeBridgeState(env_state);
}

extern "C" int snapi_bridge_unofficial_release_env_with_loop(SnapiEnvState* env_state,
                                                             uint32_t loop_id) {
  (void)loop_id;
  return DisposeBridgeState(env_state);
}

extern "C" int snapi_bridge_unofficial_collect_garbage(
    SnapiEnvState* env_state) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return unofficial_napi_collect_garbage(env);
}

extern "C" int snapi_bridge_unofficial_event_loop_checkpoint(
    SnapiEnvState* env_state,
    int mode,
    int has_runnable_work,
    uint32_t* checkpoint_state) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  uint32_t state = unofficial_napi_event_loop_checkpoint_state_none;
  const int status = unofficial_napi_event_loop_checkpoint(
      env,
      static_cast<unofficial_napi_event_loop_checkpoint_mode>(mode),
      has_runnable_work != 0,
      &state);
  if (checkpoint_state != nullptr) {
    *checkpoint_state = state;
  }
  return status;
}

extern "C" int snapi_bridge_unofficial_set_prepare_stack_trace_callback(
    SnapiEnvState* env_state,
    uint32_t callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(*bridge_state, callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_set_prepare_stack_trace_callback(env, callback);
}

extern "C" int snapi_bridge_unofficial_get_promise_details(SnapiEnvState* env_state,
                                                           uint32_t promise_id,
                                                           int32_t* state_out,
                                                           uint32_t* result_out,
                                                           int* has_result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value promise = LoadValue(*bridge_state, promise_id);
  if (promise == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  bool has_result = false;
  napi_status s =
      unofficial_napi_get_promise_details(env, promise, state_out, &result, &has_result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  if (has_result_out != nullptr) *has_result_out = has_result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_proxy_details(SnapiEnvState* env_state,
                                                         uint32_t proxy_id,
                                                         uint32_t* target_out,
                                                         uint32_t* handler_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value proxy = LoadValue(*bridge_state, proxy_id);
  if (proxy == nullptr) return napi_invalid_arg;
  napi_value target = nullptr;
  napi_value handler = nullptr;
  napi_status s = unofficial_napi_get_proxy_details(env, proxy, &target, &handler);
  if (s != napi_ok) return s;
  if (target_out != nullptr) *target_out = StoreValue(*bridge_state, target);
  if (handler_out != nullptr) *handler_out = StoreValue(*bridge_state, handler);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_preview_entries(SnapiEnvState* env_state,
                                                       uint32_t value_id,
                                                       uint32_t* entries_out,
                                                       int* is_key_value_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value entries = nullptr;
  bool is_key_value = false;
  napi_status s = unofficial_napi_preview_entries(env, value, &entries, &is_key_value);
  if (s != napi_ok) return s;
  if (entries_out != nullptr) *entries_out = StoreValue(*bridge_state, entries);
  if (is_key_value_out != nullptr) *is_key_value_out = is_key_value ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_call_sites(SnapiEnvState* env_state,
                                                      uint32_t frames,
                                                      uint32_t* callsites_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value callsites = nullptr;
  napi_status s = unofficial_napi_get_call_sites(env, frames, &callsites);
  if (s != napi_ok) return s;
  if (callsites_out != nullptr) *callsites_out = StoreValue(*bridge_state, callsites);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_arraybuffer_view_has_buffer(SnapiEnvState* env_state,
                                                                   uint32_t value_id,
                                                                   int* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_arraybuffer_view_has_buffer(env, value, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_constructor_name(SnapiEnvState* env_state,
                                                            uint32_t value_id,
                                                            uint32_t* name_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value name = nullptr;
  napi_status s = unofficial_napi_get_constructor_name(env, value, &name);
  if (s != napi_ok) return s;
  if (name_out != nullptr) *name_out = StoreValue(*bridge_state, name);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_create_private_symbol(SnapiEnvState* env_state,
                                                             const char* utf8description,
                                                             uint32_t wasm_length,
                                                             uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : static_cast<size_t>(wasm_length);
  napi_value result = nullptr;
  napi_status s =
      unofficial_napi_create_private_symbol(env, utf8description, length, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_continuation_preserved_embedder_data(
    SnapiEnvState* env_state,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result = nullptr;
  napi_status s =
      unofficial_napi_get_continuation_preserved_embedder_data(env, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_set_continuation_preserved_embedder_data(
    SnapiEnvState* env_state,
    uint32_t value_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = value_id == 0 ? nullptr : LoadValue(*bridge_state, value_id);
  if (value_id != 0 && value == nullptr) return napi_invalid_arg;
  return unofficial_napi_set_continuation_preserved_embedder_data(env, value);
}

extern "C" int snapi_bridge_unofficial_attach_env(
    SnapiEnvState* env_state,
    uint32_t /*fatal_callback_id*/,
    uint32_t /*oom_callback_id*/,
    uint64_t* accepted_hooks_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || accepted_hooks_out == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  // Guest callbacks cannot be installed as native provider function pointers.
  // Guest lifecycle stays in the Wasmer bridge, while the provider receives
  // the same single immutable attachment transition with empty native hooks.
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  return unofficial_napi_attach_env(env, &hooks, accepted_hooks_out);
}

// Keeps `env_state` allocated (not alive) until the matching unretain, so a
// holder that hands the pointer to other threads never sees it reused for a
// different env. Call while the env is alive, e.g. right after creation.
extern "C" void snapi_bridge_unofficial_retain_env(SnapiEnvState* env_state) {
  if (env_state != nullptr) env_state->holders.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void snapi_bridge_unofficial_unretain_env(SnapiEnvState* env_state) {
  if (env_state != nullptr) DropEnvStateHolder(env_state);
}

extern "C" int snapi_bridge_unofficial_terminate_execution(SnapiEnvState* env_state) {
  // V8 explicitly permits termination from another thread. Keep registry
  // membership pinned only for this non-reentrant engine control operation.
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_envs.count(env_state)) return napi_invalid_arg;
  SnapiAgentState* agent = env_state->agent;
  if (agent->closing) {
    // Releasing: stop any guest code teardown is running now (finalizers can
    // run JavaScript) and let no more start. Outside that guest code the
    // isolate may already be going away, and no JavaScript can run anyway.
    agent->stop_requested = true;
    if (agent->teardown_guest_calls == 0) return napi_ok;
  }
  return unofficial_napi_terminate_execution(env_state->env);
}

extern "C" int snapi_bridge_unofficial_cancel_terminate_execution(
    SnapiEnvState* env_state) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return unofficial_napi_cancel_terminate_execution(env);
}

extern "C" int snapi_bridge_unofficial_request_interrupt(SnapiEnvState* env_state,
                                                         uint32_t guest_env,
                                                         uint32_t wasm_fn_ptr,
                                                         uint32_t data) {
  if (wasm_fn_ptr == 0) return napi_invalid_arg;
  std::unique_lock<std::mutex> lock(g_mu);
  auto* bridge_state = env_state;
  if (!g_envs.count(bridge_state) || bridge_state->agent->closing) {
    return napi_invalid_arg;
  }
  SnapiAgentState* agent = bridge_state->agent;
  auto* request =
      new (std::nothrow) WasmInterruptRequest{bridge_state, guest_env, wasm_fn_ptr, data};
  if (request == nullptr) return napi_generic_failure;
  agent->pending_interrupts.insert(request);
  // Only the owner thread releases an env, so an owner caller needs no lock to
  // keep it alive. A foreign caller keeps registry membership pinned: the
  // engine only enqueues the callback, which runs later on the owner thread.
  if (OwnedByThisThread(bridge_state)) lock.unlock();
  napi_env env = bridge_state->env;
  napi_status s =
      unofficial_napi_request_interrupt(env, WasmInterruptCallback, request);
  if (s != napi_ok) {
    if (!lock.owns_lock()) lock.lock();
    agent->pending_interrupts.erase(request);
    delete request;
  }
  return s;
}

extern "C" int snapi_bridge_unofficial_enqueue_microtask(SnapiEnvState* env_state,
                                                         uint32_t callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value callback = LoadValue(*bridge_state, callback_id);
  if (callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_enqueue_microtask(env, callback);
}

extern "C" int snapi_bridge_unofficial_set_promise_reject_callback(SnapiEnvState* env_state,
                                                                   uint32_t callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(*bridge_state, callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_set_promise_reject_callback(env, callback);
}

extern "C" int snapi_bridge_unofficial_get_own_non_index_properties(
    SnapiEnvState* env_state,
    uint32_t value_id,
    uint32_t filter_bits,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || out_id == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status status = unofficial_napi_get_own_non_index_properties(
      env, value, filter_bits, &result);
  if (status != napi_ok) return status;
  *out_id = StoreValue(*bridge_state, result);
  return *out_id != 0 ? napi_ok : napi_generic_failure;
}

extern "C" int snapi_bridge_unofficial_set_promise_hooks(
    SnapiEnvState* env_state,
    uint32_t init_callback_id,
    uint32_t before_callback_id,
    uint32_t after_callback_id,
    uint32_t resolve_callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value init = init_callback_id == 0 ? nullptr : LoadValue(*bridge_state, init_callback_id);
  napi_value before =
      before_callback_id == 0 ? nullptr : LoadValue(*bridge_state, before_callback_id);
  napi_value after = after_callback_id == 0 ? nullptr : LoadValue(*bridge_state, after_callback_id);
  napi_value resolve =
      resolve_callback_id == 0 ? nullptr : LoadValue(*bridge_state, resolve_callback_id);
  if ((init_callback_id != 0 && init == nullptr) ||
      (before_callback_id != 0 && before == nullptr) ||
      (after_callback_id != 0 && after == nullptr) ||
      (resolve_callback_id != 0 && resolve == nullptr)) {
    return napi_invalid_arg;
  }
  return unofficial_napi_set_promise_hooks(env, init, before, after, resolve);
}

extern "C" int snapi_bridge_unofficial_get_hash_seed(SnapiEnvState* env_state,
                                                      uint64_t* hash_seed_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (
      hash_seed_out == nullptr) {
    return napi_invalid_arg;
  }
  return unofficial_napi_get_hash_seed(env, hash_seed_out);
}

extern "C" int snapi_bridge_unofficial_get_error_metadata(
    SnapiEnvState* env_state,
    uint32_t error_id,
    int mode,
    uint32_t* source_line_out,
    uint32_t* script_resource_name_out,
    uint32_t* stderr_line_out,
    uint32_t* thrown_at_out,
    int32_t* line_number_out,
    int32_t* start_column_out,
    int32_t* end_column_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value error = LoadValue(*bridge_state, error_id);
  if (error == nullptr) return napi_invalid_arg;
  unofficial_napi_error_metadata metadata{};
  napi_status s = unofficial_napi_get_error_metadata(
      env,
      error,
      static_cast<unofficial_napi_error_metadata_mode>(mode),
      &metadata);
  if (s != napi_ok) return s;
  if (source_line_out != nullptr) {
    *source_line_out = StoreValue(*bridge_state, metadata.source_line);
  }
  if (script_resource_name_out != nullptr) {
    *script_resource_name_out =
        StoreValue(*bridge_state, metadata.script_resource_name);
  }
  if (stderr_line_out != nullptr) {
    *stderr_line_out = StoreValue(*bridge_state, metadata.stderr_line);
  }
  if (thrown_at_out != nullptr) {
    *thrown_at_out = StoreValue(*bridge_state, metadata.thrown_at);
  }
  if (line_number_out != nullptr) *line_number_out = metadata.line_number;
  if (start_column_out != nullptr) *start_column_out = metadata.start_column;
  if (end_column_out != nullptr) *end_column_out = metadata.end_column;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_configure_source_maps(
    SnapiEnvState* env_state,
    int enabled,
    uint32_t callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(*bridge_state, callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_configure_source_maps(env, enabled != 0, callback);
}

extern "C" int snapi_bridge_unofficial_preserve_error_source_message(
    SnapiEnvState* env_state,
    uint32_t error_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value error = LoadValue(*bridge_state, error_id);
  if (error == nullptr) return napi_invalid_arg;
  return unofficial_napi_preserve_error_source_message(env, error);
}

extern "C" int snapi_bridge_unofficial_mark_promise_as_handled(
    SnapiEnvState* env_state,
    uint32_t promise_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value promise = LoadValue(*bridge_state, promise_id);
  if (promise == nullptr) return napi_invalid_arg;
  return unofficial_napi_mark_promise_as_handled(env, promise);
}

extern "C" int snapi_bridge_unofficial_get_heap_statistics(
    SnapiEnvState* env_state,
    unofficial_napi_heap_statistics* stats_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (
      stats_out == nullptr) {
    return napi_invalid_arg;
  }
  return unofficial_napi_get_heap_statistics(env, stats_out);
}

extern "C" int snapi_bridge_unofficial_get_heap_space_statistics(
    SnapiEnvState* env_state,
    unofficial_napi_heap_space_statistics* stats_out,
    uint32_t capacity,
    uint32_t* count_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (count_out == nullptr || (capacity > 0 && stats_out == nullptr)) {
    return napi_invalid_arg;
  }
  return unofficial_napi_get_heap_space_statistics(env, stats_out, capacity, count_out);
}

extern "C" int snapi_bridge_unofficial_get_heap_code_statistics(
    SnapiEnvState* env_state,
    unofficial_napi_heap_code_statistics* stats_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (
      stats_out == nullptr) {
    return napi_invalid_arg;
  }
  return unofficial_napi_get_heap_code_statistics(env, stats_out);
}

extern "C" int snapi_bridge_unofficial_profile_start(SnapiEnvState* env_state,
                                                      int32_t kind,
                                                      int32_t* result_out,
                                                      uint32_t* profile_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (result_out == nullptr || profile_out == nullptr) {
    return napi_invalid_arg;
  }
  unofficial_napi_profile_start_result result = unofficial_napi_profile_start_ok;
  unofficial_napi_profile profile = nullptr;
  napi_status s = unofficial_napi_profile_start(
      env, static_cast<unofficial_napi_profile_kind>(kind), &result, &profile);
  if (s != napi_ok) return s;
  *result_out = static_cast<int32_t>(result);
  *profile_out = bridge_state->agent->profile_handles.Store(
      reinterpret_cast<void*>(profile));
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_profile_stop(SnapiEnvState* env_state,
                                                     uint32_t profile_id,
                                                     uint32_t* json_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (json_out == nullptr) return napi_invalid_arg;
  auto profile = reinterpret_cast<unofficial_napi_profile>(
      bridge_state->agent->profile_handles.Load(profile_id));
  if (profile == nullptr) return napi_invalid_arg;
  napi_value json = nullptr;
  napi_status s = unofficial_napi_profile_stop(env, profile, &json);
  if (s != napi_cannot_run_js && s != napi_invalid_arg) {
    (void)bridge_state->agent->profile_handles.Take(profile_id);
  }
  if (s != napi_ok) return s;
  *json_out = json != nullptr ? StoreValue(*bridge_state, json) : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_take_heap_snapshot(
    SnapiEnvState* env_state,
    int expose_internals,
    int expose_numeric_values,
    uint32_t* json_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (json_out == nullptr) {
    return napi_invalid_arg;
  }
  unofficial_napi_heap_snapshot_options options{};
  options.expose_internals = expose_internals != 0;
  options.expose_numeric_values = expose_numeric_values != 0;
  napi_value json = nullptr;
  napi_status s = unofficial_napi_take_heap_snapshot(env, &options, &json);
  if (s != napi_ok) return s;
  *json_out = json != nullptr ? StoreValue(*bridge_state, json) : 0;
  return napi_ok;
}

extern "C" void snapi_bridge_free_buffer(void* data) {
  std::free(data);
}

extern "C" int snapi_bridge_unofficial_structured_clone(
    SnapiEnvState* env_state,
    uint32_t value_id,
    uint32_t transfer_list_id,
    uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  // A zero id means "no transfer list" (the common case: structuredClone with
  // no transfers). That is valid — StructuredCloneImpl handles a null transfer
  // list, exactly like the no-transfer entry point. Do NOT reject it.
  napi_value transfer_list =
      transfer_list_id == 0 ? nullptr : LoadValue(*bridge_state, transfer_list_id);
  napi_value result = nullptr;
  napi_status s = unofficial_napi_structured_clone(env, value, transfer_list, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_message_create(
    SnapiEnvState* env_state,
    uint32_t value_id,
    uint32_t* message_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || message_out == nullptr) return napi_invalid_arg;
  napi_value value = LoadValue(*bridge_state, value_id);
  if (value == nullptr) return napi_invalid_arg;
  unofficial_napi_message message = nullptr;
  napi_status status =
      unofficial_napi_message_create(bridge_state->env, value, &message);
  if (status != napi_ok) return status;
  uint32_t message_id;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    message_id = g_message_handles.Store(reinterpret_cast<void*>(message));
  }
  if (message_id == 0) {
    unofficial_napi_message_drop(message);
    return napi_generic_failure;
  }
  *message_out = message_id;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_message_take(
    SnapiEnvState* env_state,
    uint32_t message_id,
    uint32_t* value_out) {
  unofficial_napi_message message;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    message = reinterpret_cast<unofficial_napi_message>(g_message_handles.Take(message_id));
  }
  if (message == nullptr) return napi_invalid_arg;
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr || value_out == nullptr) {
    unofficial_napi_message_drop(message);
    return napi_invalid_arg;
  }
  napi_value value = nullptr;
  napi_status status =
      unofficial_napi_message_take(bridge_state->env, message, &value);
  if (status != napi_ok) return status;
  const uint32_t value_id = StoreValue(*bridge_state, value);
  if (value_id == 0) return napi_generic_failure;
  *value_out = value_id;
  return napi_ok;
}

extern "C" void snapi_bridge_unofficial_message_drop(uint32_t message_id) {
  unofficial_napi_message message;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    message = reinterpret_cast<unofficial_napi_message>(g_message_handles.Take(message_id));
  }
  if (message != nullptr) unofficial_napi_message_drop(message);
}

extern "C" int snapi_bridge_unofficial_notify_datetime_configuration_change(
    SnapiEnvState* env_state) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  return unofficial_napi_notify_datetime_configuration_change(env);
}

extern "C" int snapi_bridge_unofficial_create_serdes_binding(SnapiEnvState* env_state,
                                                             uint32_t* out_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_create_serdes_binding(env, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_contains_module_syntax(
    SnapiEnvState* env_state,
    uint32_t code_id,
    uint32_t filename_id,
    uint32_t resource_name_id,
    int cjs_var_in_scope,
    int* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value code = LoadValue(*bridge_state, code_id);
  napi_value filename = LoadValue(*bridge_state, filename_id);
  napi_value resource_name = resource_name_id == 0 ? nullptr : LoadValue(*bridge_state, resource_name_id);
  if (code == nullptr || filename == nullptr) return napi_invalid_arg;
  if (resource_name_id != 0 && resource_name == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_contextify_contains_module_syntax(
      env, code, filename, resource_name, cjs_var_in_scope != 0, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_make_context(
    SnapiEnvState* env_state,
    uint32_t sandbox_or_symbol_id,
    uint32_t name_id,
    uint32_t origin_id,
    int allow_code_gen_strings,
    int allow_code_gen_wasm,
    int own_microtask_queue,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value sandbox_or_symbol = LoadValue(*bridge_state, sandbox_or_symbol_id);
  napi_value name = LoadValue(*bridge_state, name_id);
  napi_value origin = origin_id == 0 ? nullptr : LoadValue(*bridge_state, origin_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(*bridge_state, host_defined_option_id);
  if (sandbox_or_symbol == nullptr || name == nullptr) return napi_invalid_arg;
  if (origin_id != 0 && origin == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_make_context(
      env,
      sandbox_or_symbol,
      name,
      origin,
      allow_code_gen_strings != 0,
      allow_code_gen_wasm != 0,
      own_microtask_queue != 0,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_run_script(
    SnapiEnvState* env_state,
    uint32_t sandbox_or_null_id,
    uint32_t source_text_id,
    uint32_t source_bytecode_id,
    uint32_t filename_id,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    int display_errors,
    int break_on_sigint,
    int break_on_first_line,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value sandbox_or_null = sandbox_or_null_id == 0 ? nullptr : LoadValue(*bridge_state, sandbox_or_null_id);
  napi_value source_text = source_text_id == 0 ? nullptr : LoadValue(*bridge_state, source_text_id);
  unofficial_napi_bytecode source_bytecode =
      LoadBytecodeHandle(*bridge_state, source_bytecode_id);
  napi_value filename = LoadValue(*bridge_state, filename_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(*bridge_state, host_defined_option_id);
  if (sandbox_or_null_id != 0 && sandbox_or_null == nullptr) return napi_invalid_arg;
  if ((source_text == nullptr && source_bytecode == nullptr) || filename == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  const unofficial_napi_js_source source = source_text != nullptr
                                               ? unofficial_napi_js_source_from_text(source_text)
                                               : unofficial_napi_js_source_from_bytecode(source_bytecode);
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_run_script(
      env,
      sandbox_or_null,
      &source,
      filename,
      line_offset,
      column_offset,
      timeout,
      display_errors != 0,
      break_on_sigint != 0,
      break_on_first_line != 0,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_compile_function(
    SnapiEnvState* env_state,
    uint32_t source_text_id,
    uint32_t source_bytecode_id,
    uint32_t filename_id,
    int32_t line_offset,
    int32_t column_offset,
    uint32_t parsing_context_id,
    uint32_t context_extensions_id,
    uint32_t params_id,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value source_text = source_text_id == 0 ? nullptr : LoadValue(*bridge_state, source_text_id);
  unofficial_napi_bytecode source_bytecode =
      LoadBytecodeHandle(*bridge_state, source_bytecode_id);
  napi_value filename = LoadValue(*bridge_state, filename_id);
  napi_value parsing_context = parsing_context_id == 0 ? nullptr : LoadValue(*bridge_state, parsing_context_id);
  napi_value context_extensions =
      context_extensions_id == 0 ? nullptr : LoadValue(*bridge_state, context_extensions_id);
  napi_value params = params_id == 0 ? nullptr : LoadValue(*bridge_state, params_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(*bridge_state, host_defined_option_id);
  if ((source_text == nullptr && source_bytecode == nullptr) || filename == nullptr) return napi_invalid_arg;
  if (parsing_context_id != 0 && parsing_context == nullptr) return napi_invalid_arg;
  if (context_extensions_id != 0 && context_extensions == nullptr) return napi_invalid_arg;
  if (params_id != 0 && params == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  const unofficial_napi_js_source source = source_text != nullptr
                                               ? unofficial_napi_js_source_from_text(source_text)
                                               : unofficial_napi_js_source_from_bytecode(source_bytecode);
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_compile_function(
      env,
      &source,
      filename,
      line_offset,
      column_offset,
      parsing_context,
      context_extensions,
      params,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_bytecode_open(
    SnapiEnvState* env_state,
    uint32_t source_text_id,
    uint32_t filename_id,
    int32_t shape,
    uint32_t params_id,
    uint32_t host_defined_option_id,
    int32_t line_offset,
    int32_t column_offset,
    const uint8_t* cache_bytes,
    size_t cache_byte_length,
    uint8_t has_cache,
    uint8_t cache_policy,
    uint32_t* bytecode_out,
    uint8_t* cache_rejected_out,
    uint8_t* can_parse_as_module_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value source_text = LoadValue(*bridge_state, source_text_id);
  napi_value filename = LoadValue(*bridge_state, filename_id);
  napi_value params = params_id == 0 ? nullptr : LoadValue(*bridge_state, params_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(*bridge_state, host_defined_option_id);
  if (source_text == nullptr || filename == nullptr) return napi_invalid_arg;
  if (params_id != 0 && params == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  unofficial_napi_bytecode_open_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  options.source_text = source_text;
  options.filename = filename;
  options.shape = shape;
  options.params_or_undefined = params;
  options.host_defined_option_id = host_defined_option;
  options.line_offset = line_offset;
  options.column_offset = column_offset;
  options.cache_bytes = cache_bytes;
  options.cache_byte_length = cache_byte_length;
  options.has_cache = has_cache;
  options.cache_policy = cache_policy;
  unofficial_napi_bytecode_open_result result{};
  napi_status s = unofficial_napi_bytecode_open(env, &options, &result);
  if (cache_rejected_out != nullptr) *cache_rejected_out = result.cache_rejected;
  if (can_parse_as_module_out != nullptr) {
    *can_parse_as_module_out = result.can_parse_as_module;
  }
  if (s != napi_ok) return s;
  if (bytecode_out != nullptr) {
    *bytecode_out = StoreBytecodeHandle(*bridge_state, result.bytecode);
  }
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_bytecode_serialize(
    SnapiEnvState* env_state,
    uint32_t bytecode_id,
    uint32_t* buffer_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_bytecode bytecode = LoadBytecodeHandle(*bridge_state, bytecode_id);
  if (bytecode == nullptr) return napi_invalid_arg;
  napi_value buffer = nullptr;
  napi_status s = unofficial_napi_bytecode_serialize(env, bytecode, &buffer);
  if (s != napi_ok) return s;
  if (buffer_out != nullptr) *buffer_out = StoreValue(*bridge_state, buffer);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_bytecode_release(
    SnapiEnvState* env_state,
    uint32_t bytecode_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_bytecode bytecode = LoadBytecodeHandle(*bridge_state, bytecode_id);
  if (bytecode == nullptr) return napi_invalid_arg;
  napi_status s = unofficial_napi_bytecode_release(env, bytecode);
  RemoveBytecodeHandle(*bridge_state, bytecode_id);
  return s;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create(
    SnapiEnvState* env_state,
    int32_t kind,
    uint32_t wrapper_id,
    uint32_t url_id,
    uint32_t context_id,
    uint32_t source_text_id,
    uint32_t source_bytecode_id,
    int32_t line_offset,
    int32_t column_offset,
    uint32_t host_defined_option_id,
    uint32_t export_names_id,
    uint32_t synthetic_eval_steps_id,
    uint32_t* handle_out,
    uint32_t* requests_out,
    uint8_t* has_top_level_await_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value wrapper = LoadValue(*bridge_state, wrapper_id);
  napi_value url = LoadValue(*bridge_state, url_id);
  napi_value context = context_id == 0 ? nullptr : LoadValue(*bridge_state, context_id);
  napi_value source_text = source_text_id == 0 ? nullptr : LoadValue(*bridge_state, source_text_id);
  unofficial_napi_bytecode source_bytecode =
      LoadBytecodeHandle(*bridge_state, source_bytecode_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(*bridge_state, host_defined_option_id);
  napi_value export_names =
      export_names_id == 0 ? nullptr : LoadValue(*bridge_state, export_names_id);
  napi_value synthetic_eval_steps = synthetic_eval_steps_id == 0
                                        ? nullptr
                                        : LoadValue(*bridge_state, synthetic_eval_steps_id);
  if (wrapper == nullptr || url == nullptr) return napi_invalid_arg;
  if (context_id != 0 && context == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  if (export_names_id != 0 && export_names == nullptr) return napi_invalid_arg;
  if (synthetic_eval_steps_id != 0 && synthetic_eval_steps == nullptr) {
    return napi_invalid_arg;
  }

  unofficial_napi_js_source source{};
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = static_cast<unofficial_napi_module_kind>(kind);
  options.wrapper = wrapper;
  options.url = url;
  options.context_or_undefined = context;
  switch (options.kind) {
    case unofficial_napi_module_source_text:
      if (source_text == nullptr && source_bytecode == nullptr) return napi_invalid_arg;
      source = source_text != nullptr
                   ? unofficial_napi_js_source_from_text(source_text)
                   : unofficial_napi_js_source_from_bytecode(source_bytecode);
      options.payload.source_text.source = &source;
      options.payload.source_text.line_offset = line_offset;
      options.payload.source_text.column_offset = column_offset;
      options.payload.source_text.host_defined_option_id = host_defined_option;
      break;
    case unofficial_napi_module_synthetic:
      if (export_names == nullptr || synthetic_eval_steps == nullptr) {
        return napi_invalid_arg;
      }
      options.payload.synthetic.export_names = export_names;
      options.payload.synthetic.synthetic_evaluation_steps = synthetic_eval_steps;
      break;
    default:
      return napi_invalid_arg;
  }

  unofficial_napi_module_create_result result{};
  napi_status s = unofficial_napi_module_wrap_create(env, &options, &result);
  if (s != napi_ok) return s;
  if (result.module == nullptr || result.module_requests == nullptr) {
    if (result.module != nullptr) {
      (void)unofficial_napi_module_wrap_destroy(env, result.module);
    }
    return napi_generic_failure;
  }
  if (handle_out != nullptr) {
    *handle_out = StoreModuleWrapHandle(*bridge_state, result.module);
  }
  if (requests_out != nullptr) {
    *requests_out = StoreValue(*bridge_state, result.module_requests);
  }
  if (has_top_level_await_out != nullptr) {
    *has_top_level_await_out = result.has_top_level_await ? 1 : 0;
  }
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_destroy(SnapiEnvState* env_state,
                                                           uint32_t handle_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_status s = unofficial_napi_module_wrap_destroy(env, module);
  if (s == napi_ok) RemoveModuleWrapHandle(*bridge_state, handle_id);
  return s;
}

extern "C" int snapi_bridge_unofficial_module_wrap_link(SnapiEnvState* env_state,
                                                        uint32_t handle_id,
                                                        uint32_t count,
                                                        const uint32_t* linked_handle_ids) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  std::vector<unofficial_napi_module> linked_modules(count, nullptr);
  for (uint32_t i = 0; i < count; ++i) {
    unofficial_napi_module linked =
        linked_handle_ids != nullptr
            ? LoadModuleWrapHandle(*bridge_state, linked_handle_ids[i])
            : nullptr;
    if (linked == nullptr) return napi_invalid_arg;
    linked_modules[i] = linked;
  }
  return unofficial_napi_module_wrap_link(
      env, module, count, count == 0 ? nullptr : linked_modules.data());
}

extern "C" int snapi_bridge_unofficial_module_wrap_instantiate(SnapiEnvState* env_state,
                                                               uint32_t handle_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_instantiate(env, module);
}

extern "C" int snapi_bridge_unofficial_module_wrap_evaluate(SnapiEnvState* env_state,
                                                            uint32_t handle_id,
                                                            int64_t timeout,
                                                            int break_on_sigint,
                                                            uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_evaluate(
      env, module, timeout, break_on_sigint != 0, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_evaluate_sync(SnapiEnvState* env_state,
                                                                 uint32_t handle_id,
                                                                 uint32_t filename_id,
                                                                 uint32_t parent_filename_id,
                                                                 uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value filename = filename_id == 0 ? nullptr : LoadValue(*bridge_state, filename_id);
  napi_value parent_filename =
      parent_filename_id == 0 ? nullptr : LoadValue(*bridge_state, parent_filename_id);
  if ((filename_id != 0 && filename == nullptr) ||
      (parent_filename_id != 0 && parent_filename == nullptr)) {
    return napi_invalid_arg;
  }
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_evaluate_sync(
      env, module, filename, parent_filename, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_namespace(SnapiEnvState* env_state,
                                                                 uint32_t handle_id,
                                                                 uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_namespace(env, module, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_state(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    int32_t* status_out,
    uint32_t* error_out,
    int* has_async_graph_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  if (status_out == nullptr && error_out == nullptr &&
      has_async_graph_out == nullptr) {
    return napi_invalid_arg;
  }
  napi_value error = nullptr;
  bool has_async_graph = false;
  napi_status s = unofficial_napi_module_wrap_get_state(
      env,
      module,
      status_out,
      error_out != nullptr ? &error : nullptr,
      has_async_graph_out != nullptr ? &has_async_graph : nullptr);
  if (s != napi_ok) return s;
  if (error_out != nullptr) *error_out = StoreValue(*bridge_state, error);
  if (has_async_graph_out != nullptr) {
    *has_async_graph_out = has_async_graph ? 1 : 0;
  }
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_check_unsettled_top_level_await(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    int warnings,
    int* settled_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  if (settled_out == nullptr) return napi_invalid_arg;
  unofficial_napi_module module =
      LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  bool settled = true;
  napi_status s = unofficial_napi_module_wrap_check_unsettled_top_level_await(
      env, module, warnings != 0, &settled);
  if (s != napi_ok) return s;
  *settled_out = settled ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_export(SnapiEnvState* env_state,
                                                              uint32_t handle_id,
                                                              uint32_t export_name_id,
                                                              uint32_t export_value_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  napi_value export_name = LoadValue(*bridge_state, export_name_id);
  napi_value export_value = export_value_id == 0 ? nullptr : LoadValue(*bridge_state, export_value_id);
  if (module == nullptr || export_name == nullptr) return napi_invalid_arg;
  if (export_value_id != 0 && export_value == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_export(env, module, export_name, export_value);
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_module_source_object(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    uint32_t source_object_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  napi_value source_object = source_object_id == 0 ? nullptr : LoadValue(*bridge_state, source_object_id);
  if (module == nullptr) return napi_invalid_arg;
  if (source_object_id != 0 && source_object == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_module_source_object(env, module, source_object);
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_module_source_object(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_module_source_object(env, module, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_cached_data(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_cached_data(env, module, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_hooks(
    SnapiEnvState* env_state,
    uint32_t import_callback_id,
    uint32_t import_meta_callback_id) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  napi_value import_callback = import_callback_id == 0
                                   ? nullptr
                                   : LoadValue(*bridge_state, import_callback_id);
  napi_value import_meta_callback = import_meta_callback_id == 0
                                        ? nullptr
                                        : LoadValue(*bridge_state, import_meta_callback_id);
  if ((import_callback_id != 0 && import_callback == nullptr) ||
      (import_meta_callback_id != 0 && import_meta_callback == nullptr)) {
    return napi_invalid_arg;
  }
  const unofficial_napi_module_hooks hooks = {
      sizeof(unofficial_napi_module_hooks),
      UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION,
      import_callback,
      import_meta_callback,
  };
  return unofficial_napi_module_wrap_set_hooks(env, &hooks);
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_required_module_facade(
    SnapiEnvState* env_state,
    uint32_t handle_id,
    uint32_t* result_out) {
  auto* bridge_state = RequireEnvState(env_state);
  if (bridge_state == nullptr) return napi_invalid_arg;
  napi_env env = bridge_state->env;
  unofficial_napi_module module = LoadModuleWrapHandle(*bridge_state, handle_id);
  if (module == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_required_module_facade(env, module, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(*bridge_state, result);
  return napi_ok;
}

// ============================================================
// Cleanup
// ============================================================

extern "C" void snapi_bridge_dispose() {
  // An embedder may clean up its owner thread; it must never destroy another
  // thread's entered isolate or persistent V8 scopes. Normal worker shutdown
  // explicitly releases each env on its owner before joining that thread.
  // Disposing an env disposes its agent and forgets every env on it, so take
  // one owned env at a time instead of iterating a snapshot. Agents already
  // closing are being released further up this thread's stack.
  while (t_owned_envs != nullptr) {
    SnapiEnvState* next = nullptr;
    for (auto* state : *t_owned_envs) {
      if (!state->agent->closing) {
        next = state;
        break;
      }
    }
    if (next == nullptr) break;
    (void)DisposeBridgeState(next);
  }
  // Opaque messages are owned by queues, not by a source env. They may remain
  // live between source release and destination creation, so only take/drop
  // retires them. Queue shutdown must explicitly drop its remaining tokens.
}
