#ifndef NAPI_V8_ENV_H_
#define NAPI_V8_ENV_H_

#include <array>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
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

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);

struct napi_buffer_record__;
struct napi_callback_payload__;
struct napi_deferred__;
struct napi_env__;
struct napi_env_cleanup_hook__;
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
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  std::vector<napi_env_cleanup_hook__*> env_cleanup_hooks;
  uint64_t env_cleanup_hook_counter = 0;
  std::vector<napi_buffer_record__*> buffer_records;
  std::unordered_set<napi_external_backing_store_hint__*> external_backing_store_hints;
  napi_ref_tracker__::RefList reflist;
  napi_ref_tracker__::RefList finalizing_reflist;
  std::unordered_set<napi_ref_tracker__*> pending_finalizers;
  std::unordered_set<napi_buffer_record__*> pending_buffer_finalizers;
  bool finalization_scheduled = false;
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

napi_status napi_v8_set_last_error(napi_env env,
                                   napi_status status,
                                   const char* message);

napi_status napi_v8_clear_last_error(napi_env env);

void napi_v8_set_last_exception(
    napi_env env,
    v8::Local<v8::Value> exception,
    v8::Local<v8::Message> message = v8::Local<v8::Message>());

napi_value napi_v8_wrap_value(napi_env env, v8::Local<v8::Value> value);
v8::Local<v8::Value> napi_v8_unwrap_value(napi_value value);
void napi_v8_finalize_buffer_records(napi_env env);

#endif  // NAPI_V8_ENV_H_
