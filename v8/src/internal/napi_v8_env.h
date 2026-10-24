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

// A realm. It owns nothing but its context: everything else N-API keeps is
// on its agent and shared by every env of the isolate.
struct napi_env__ {
  explicit napi_env__(v8::Local<v8::Context> context, int32_t module_api_version);
  ~napi_env__();

  v8::Local<v8::Context> context() const;
  // Calls `cb` with this env, in this env's realm.
  void CallFinalizer(node_api_basic_finalize cb, void* data, void* hint);
  void InvokeFinalizerFromGC(napi_ref_tracker__* finalizer);
  void EnqueueFinalizer(napi_ref_tracker__* finalizer);
  void EnqueueBufferFinalizer(napi_buffer_record__* record);
  void DequeueFinalizer(napi_ref_tracker__* finalizer);
  void DrainFinalizerQueue();

  template <typename T, typename... Args>
  T* allocate(Args&&... args) {
    return agent->factory->allocate<T>(std::forward<Args>(args)...);
  }

  template <typename T>
  void release(T* value) {
    agent->factory->release<T>(value);
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
  // The realm's own microtask queue (unofficial_napi_realm_options
  // .own_microtask_queue); null when it uses the isolate's default queue.
  std::unique_ptr<v8::MicrotaskQueue> own_microtask_queue;
};

// The env bound to `context`, or null.
napi_env NapiV8EnvForContext(v8::Local<v8::Context> context);

// The env of `context`'s realm, else `isolate`'s principal env.
inline napi_env NapiV8EnvForContextOrPrincipal(v8::Isolate* isolate,
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
