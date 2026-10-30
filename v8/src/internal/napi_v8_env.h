#ifndef NAPI_V8_ENV_H_
#define NAPI_V8_ENV_H_

#include <array>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
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

struct napi_env__ {
  explicit napi_env__(v8::Local<v8::Context> context, int32_t module_api_version);
  ~napi_env__();

  v8::Local<v8::Context> context() const;
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
  // The env's realm. Never changes.
  v8::Global<v8::Context> context_ref;
  // The env this realm env was created from (unofficial_napi_create_realm);
  // null for the principal env. Hooks an env has not set fall back to it.
  napi_env parent = nullptr;
  // The realm's own microtask queue, owned by its context record; null when
  // it uses the isolate's default queue.
  v8::MicrotaskQueue* own_microtask_queue = nullptr;
  napi::error_state__ error_state;
  std::vector<void*> open_handle_scope_stack;
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

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
  napi::periodic_gate__ lifetime_stats_gate_{2000};
  napi::periodic_gate__ lifetime_string_symbol_values_gate_{10000};
#endif

 private:
  v8impl::detail::napi_factory__ factory_;
};

// The env bound to `context`, or null.
napi_env NapiV8EnvForContext(v8::Local<v8::Context> context);

// The env of `context`'s realm, else `isolate`'s principal env.
inline napi_env NapiV8RealmEnvOrPrincipal(v8::Isolate* isolate,
                                          v8::Local<v8::Context> context) {
  napi_env env = NapiV8EnvForContext(context);
  if (env != nullptr) return env;
  napi_agent__* agent = napi_agent__::From(isolate);
  return agent != nullptr ? agent->principal_env : nullptr;
}

// Enters `env`'s realm for V8 calls that create objects in the current
// context, unless that realm is already current.
class NapiV8RealmScope {
 public:
  explicit NapiV8RealmScope(napi_env env) {
    v8::Local<v8::Context> context = env->context();
    if (env->isolate->GetCurrentContext() != context) scope_.emplace(context);
  }

 private:
  std::optional<v8::Context::Scope> scope_;
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
