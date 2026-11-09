#ifndef NAPI_V8_ENV_H_
#define NAPI_V8_ENV_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <v8.h>

#include "js_native_api.h"
#include "napi_error_state.h"
#include "napi_periodic_gate.h"
#include "internal/napi_factory.h"
#include "internal/napi_lifetime_tracker.h"
#include "internal/napi_ref_tracker.h"
#include "internal/napi_v8_agent.h"
#include "unofficial_napi.h"

namespace v8 { class SnapshotCreator; }

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);

struct napi_buffer_record__;
struct napi_callback_payload__;
struct napi_deferred__;
struct napi_env__;
struct napi_env_cleanup_hook__;
struct napi_env_drain_token__;
struct napi_escapable_handle_scope__;
struct napi_external_backing_store_hint__;
struct napi_external_wrapper__;
struct napi_handle_scope__;
struct napi_ref__;
struct napi_ref_with_data__;
struct napi_ref_with_finalizer__;
static_assert(sizeof(v8::Local<v8::Value>) == sizeof(napi_value),
              "Cannot convert between v8::Local<v8::Value> and napi_value");

inline napi_value JsValueFromV8LocalValue(v8::Local<v8::Value> local) {
  return reinterpret_cast<napi_value>(*local);
}

inline v8::Local<v8::Value> V8LocalValueFromJsValue(napi_value value) {
  v8::Local<v8::Value> local;
  std::memcpy(static_cast<void*>(&local), &value, sizeof(value));
  return local;
}

// Frees an external backing store's memory. Runs in V8's deleter, on any
// thread, so it must not use V8 or an env.
typedef void (*napi_v8_external_free_callback)(void* data, void* hint);

// An external backing store's finalizer, claimed by the store's deleter and
// waiting to run on the env's thread. The teardown also claims stores' free
// callbacks: `free_cb` is set instead of `cb` for those.
struct napi_external_finalizer__ {
  node_api_basic_finalize cb = nullptr;
  void* data = nullptr;
  void* hint = nullptr;
  napi_v8_external_free_callback free_cb = nullptr;
  // The store's id (napi_external_backing_store_hint__::id), 0 for none.
  uint64_t id = 0;
};

// The array buffer of an external backing store with a user finalizer, held
// weakly, so the env's release can detach it before the finalizer runs.
struct napi_external_array_buffer__ {
  // The store's hint. Compared, and read only under NapiV8ExternalHintsMutex()
  // while it is still in the env's external_backing_store_hints: its deleter
  // may have freed it.
  napi_external_backing_store_hint__* hint = nullptr;
  v8::Global<v8::ArrayBuffer> array_buffer;
};

struct napi_callback_info__ {
  virtual ~napi_callback_info__() = default;
  virtual size_t argc() const = 0;
  virtual void args(napi_value* argv, size_t argc) const = 0;
  virtual napi_value this_arg() const = 0;
  virtual napi_value new_target() const = 0;
  virtual void* data() const = 0;
};

// A realm made with unofficial_napi_create_realm (a vm context). Its context
// is held weakly: the realm lives while its global, or anything made in it,
// is reachable. The env frees the record once the context is gone, and only
// then the realm's own microtask queue, which V8 holds by raw pointer for the
// context's lifetime.
struct unofficial_napi_realm__ {
  napi_env env = nullptr;
  v8::Global<v8::Context> context;
  std::unique_ptr<v8::MicrotaskQueue> own_microtask_queue;
  // The record is the realm's context token. True while the realm has one,
  // until the unassign hook, which runs before the record is freed.
  bool token_assigned = false;
  // Templates assign static IDs to functions for serialization. The opaque
  // realm owner stays with this realm and is never serialized.
  bool snapshot_template = false;
  bool snapshot_added = false;
  uint64_t embedder_data = 0;
};

// A realm N-API acts in: a scope opened with unofficial_napi_enter_realm, or
// a native callback that is running. The innermost frame is the current
// realm.
struct napi_realm_frame__ {
  // The scope's token; 0 for a callback.
  uint64_t scope = 0;
  // The realm; null for the env's own.
  unofficial_napi_realm__* realm = nullptr;
  // A scope enters its realm's context and holds it until it is left. A
  // callback's realm is V8's current context already, and its function keeps
  // it alive.
  v8::Global<v8::Context> entered;
};

struct NapiV8SnapshotArtifact;

struct napi_env__ {
  explicit napi_env__(v8::Local<v8::Context> context, int32_t module_api_version);
  ~napi_env__();

  // The current realm's context: the innermost realm frame's, else the env's
  // own. N-API calls create what they make there.
  v8::Local<v8::Context> context() const;
  // The env's own realm.
  v8::Local<v8::Context> main_context() const;
  // The current realm; null for the env's own.
  unofficial_napi_realm__* current_realm() const;
  // Pushes the frame of a callback running in `realm` (null: the env's own)
  // and returns the depth PopRealmFrames restores.
  size_t PushRealmFrame(unofficial_napi_realm__* realm);
  // Pops the frames above `depth`, leaving the contexts their scopes entered.
  void PopRealmFrames(size_t depth);
  void CallFinalizer(node_api_basic_finalize cb, void* data, void* hint);
  void InvokeFinalizerFromGC(napi_ref_tracker__* finalizer);
  void EnqueueFinalizer(napi_ref_tracker__* finalizer);
  void EnqueueBufferFinalizer(napi_buffer_record__* record);
  void DequeueFinalizer(napi_ref_tracker__* finalizer);
  void DrainFinalizerQueue();

  template <typename T, typename... Args>
  T* allocate(Args&&... args) {
    return factory_.allocate<T>(std::forward<Args>(args)...);
  }

  template <typename T>
  void release(T* value) {
    factory_.release<T>(value);
  }

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  bool should_dump_lifetime_stats(int64_t now_ms);
  bool should_dump_lifetime_string_symbol_values(int64_t now_ms);
#endif

  v8::Isolate* isolate = nullptr;
  // The agent this env belongs to. Set at creation and never changed.
  napi_agent__* agent = nullptr;
  // Set only when the env had to create its own agent (an isolate the
  // provider did not set up).
  std::unique_ptr<napi_agent__> owned_agent;
  // The env's own realm. Never changes.
  v8::Global<v8::Context> context_ref;
  // The realms N-API acts in, innermost last. Empty means the env's own.
  std::vector<napi_realm_frame__> realm_frames;
  // The last scope token unofficial_napi_enter_realm handed out.
  uint64_t last_realm_scope = 0;
  napi::error_state__ error_state;
  std::vector<void*> open_handle_scope_stack;
  v8::Global<v8::Value> last_exception;
  v8::Global<v8::Message> last_exception_message;
  std::string last_exception_source_line;
  std::string last_exception_thrown_at;
  // The pending exception is the null a TryCatch records for a termination.
  // Nothing threw it, so cancelling the termination drops it, as QuickJS
  // drops its uncatchable error then.
  bool last_exception_is_termination = false;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  // Private bridge hooks release callback records for dynamic bindings when
  // V8 collects their payloads. Static snapshot bindings live with the env.
  using CallbackDataHook = void (*)(napi_env, void*, void*);
  CallbackDataHook callback_data_retain = nullptr;
  CallbackDataHook callback_data_release = nullptr;
  void* callback_data_hooks_user = nullptr;
  // A builder uses static callback IDs in place of External payloads so
  // template functions can be serialized. Restore fills the table before
  // those callbacks run.
  bool snapshot_builder = false;
  // The env owner retains these bytes through isolate disposal. A rejected
  // callback manifest clears this pointer without releasing the bytes.
  const NapiV8SnapshotArtifact* snapshot_artifact = nullptr;
  bool snapshot_finalized = false;
  bool snapshot_poisoned = false;
  uint32_t snapshot_context_count = 0;
  v8::SnapshotCreator* snapshot_creator = nullptr;
  std::string snapshot_manifest;
  std::string snapshot_guest_data;
  using SnapshotCallback = std::tuple<napi_callback, void*, uint32_t>;
  std::vector<SnapshotCallback> snapshot_static_table;
  std::map<SnapshotCallback, uint32_t> snapshot_static_index;
  std::vector<napi_env_cleanup_hook__*> env_cleanup_hooks;
  uint64_t env_cleanup_hook_counter = 0;
  std::vector<napi_buffer_record__*> buffer_records;
  std::unordered_set<napi_external_backing_store_hint__*> external_backing_store_hints;
  // The array buffers of the external stores with a user finalizer, by store
  // id, so in the order they were made. An entry goes when its finalizer runs.
  // Env thread only.
  std::map<uint64_t, napi_external_array_buffer__> external_array_buffers;
  uint64_t last_external_id = 0;
  napi_ref_tracker__::RefList reflist;
  napi_ref_tracker__::RefList finalizing_reflist;
  std::unordered_set<napi_ref_tracker__*> pending_finalizers;
  std::unordered_set<napi_buffer_record__*> pending_buffer_finalizers;
  bool finalization_scheduled = false;
  // What each drain the env posts to the embedder's hook holds instead of the
  // env: the hook may keep a drain past the release and run it then. The
  // release clears the token's env, so such a drain does nothing.
  napi_env_drain_token__* drain_token = nullptr;
  // External backing-store finalizers their deleters queued for the env's
  // thread. Deleters run on any thread, so the fields below are guarded by
  // NapiV8ExternalHintsMutex(); `has_external_finalizers` is also read
  // without it, so a drain skips the lock when nothing is queued.
  std::vector<napi_external_finalizer__> external_finalizers;
  std::atomic<bool> has_external_finalizers{false};
  // A deleter posted a drain for them.
  bool external_finalizer_drain_posted = false;
  // Deleters in the embedder's hook, posting a drain. The env's teardown
  // waits for them.
  int external_drain_posts_in_flight = 0;
  std::condition_variable external_drain_posts_done;
  // The env is being torn down: deleters queue without posting, and the
  // teardown runs what they queue.
  bool releasing = false;
  // Realm hooks: unofficial_napi_set_promise_reject_callback and
  // unofficial_napi_set_promise_hooks (init, before, after, resolve).
  v8::Global<v8::Function> promise_reject_callback;
  std::array<v8::Global<v8::Function>, 4> promise_hooks;
  // The realms this env made, live or awaiting release.
  std::vector<std::unique_ptr<unofficial_napi_realm__>> realms;

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  napi::periodic_gate__ lifetime_stats_gate_{2000};
  napi::periodic_gate__ lifetime_string_symbol_values_gate_{10000};
#endif

 private:
  v8impl::detail::napi_factory__ factory_;
};

inline bool NapiV8AllowsEnvOperations(napi_env env) {
  return env != nullptr && env->isolate != nullptr && !env->snapshot_finalized;
}

// Runs the enclosing code in a realm (null: the env's own) and closes any
// scope it leaves open.
class napi_realm_frame_scope__ {
 public:
  napi_realm_frame_scope__(napi_env env, unofficial_napi_realm__* realm)
      : env_(env), depth_(env->PushRealmFrame(realm)) {}
  ~napi_realm_frame_scope__() { env_->PopRealmFrames(depth_); }
  napi_realm_frame_scope__(const napi_realm_frame_scope__&) = delete;
  napi_realm_frame_scope__& operator=(const napi_realm_frame_scope__&) = delete;

 private:
  napi_env env_;
  size_t depth_;
};

// Guards what backing-store deleters share with the env's thread: the
// external hints, the hints' env pointers, the envs' queued external
// finalizers, and the enqueue hook a deleter posts their drain with.
std::mutex& NapiV8ExternalHintsMutex();

napi_status napi_v8_set_last_error(napi_env env,
                                   napi_status status,
                                   const char* message);

napi_status napi_v8_clear_last_error(napi_env env);

void napi_v8_set_last_exception(
    napi_env env,
    v8::Local<v8::Value> exception,
    v8::Local<v8::Message> message = v8::Local<v8::Message>());

// Drops N-API's pending exception if it stands for a termination: what
// unofficial_napi_cancel_terminate_execution does with it.
void napi_v8_drop_pending_termination(napi_env env);

// Throws `exception` into V8 unless V8 is terminating, and returns whether it
// threw. Making an exception can run JS that V8 terminates (an inherited
// `code` setter, a getter, the source-map callback), and throwing would then
// replace the termination with a catchable exception.
bool napi_v8_throw_unless_terminating(v8::Isolate* isolate, v8::Local<v8::Value> exception);

// Whether the env terminates: the embedder terminated it
// (unofficial_napi_terminate_execution) and has not cancelled that, or V8 is
// unwinding with a termination. Calls into JS fail meanwhile.
bool napi_v8_terminating(napi_env env);

// For an interrupt callback (unofficial_napi_request_interrupt) that returns
// while the env terminates. V8 runs it at a stack check and then goes on with
// the interrupted JS whatever it left: StackGuard::HandleInterrupts ignores
// an exception there, a termination too, so the JS would run on with nothing
// to stop it. This asks V8 for the termination again, so it acts on it at
// its next stack check, and drops what the callback left pending, as after a
// native callback.
void napi_v8_keep_terminating_after_interrupt(napi_env env);

// What a call refused while the env terminates returns:
// napi_pending_exception, with the null a TryCatch records for a termination
// pending unless something else is, as QuickJS leaves its uncatchable error
// pending.
napi_status napi_v8_fail_terminating(napi_env env);

// Like Node's NAPI_PREAMBLE: while the env terminates, calls that enter V8 or
// may run JS fail without entering V8. Entering V8 clears the termination
// (V8 13.6's PREPARE_FOR_EXECUTION does), and JS run then runs on with
// nothing to stop it.
#define NAPI_V8_RETURN_IF_TERMINATING(env)                                   \
  do {                                                                       \
    if (napi_v8_terminating(env)) return napi_v8_fail_terminating(env);      \
  } while (0)

// For a V8 host callback (a module hook, say) that ran JS through N-API and
// failed: moves N-API's pending exception into V8, as Node's callbacks
// rethrow what their TryCatch caught, and returns true. While the env
// terminates it drops what is pending instead (throwing would replace the
// termination with a catchable exception) and makes V8 unwind with the
// termination, which V8 may have forgotten. Returns true then too: either
// way V8 now has something to unwind with, so the callback may return an
// empty result. Returns false when nothing was pending and the env is not
// terminating.
bool napi_v8_rethrow_pending_exception(napi_env env);

napi_value napi_v8_wrap_value(napi_env env, v8::Local<v8::Value> value);
v8::Local<v8::Value> napi_v8_unwrap_value(napi_value value);
void napi_v8_finalize_buffer_records(napi_env env);

// napi_create_external_arraybuffer and napi_create_external_buffer for memory
// that only needs freeing. `free_cb` runs in the backing store's deleter,
// wherever V8 frees it, so the memory comes back as V8 collects; a user
// finalizer instead waits for the env's thread. A store still alive at the
// env's release is freed then, and not again. Ownership of `free_hint`
// passes only on success.
napi_status napi_v8_create_external_arraybuffer_with_free(
    napi_env env, void* data, size_t byte_length, napi_v8_external_free_callback free_cb,
    void* free_hint, napi_value* result);
napi_status napi_v8_create_external_buffer_with_free(
    napi_env env, size_t length, void* data, napi_v8_external_free_callback free_cb,
    void* free_hint, napi_value* result);

#endif  // NAPI_V8_ENV_H_
