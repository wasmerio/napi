#ifndef NAPI_V8_ENV_H_
#define NAPI_V8_ENV_H_

#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <v8.h>

#include "js_native_api.h"
#include "internal/napi_ref_tracker.h"
#include "unofficial_napi.h"

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);

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
  void DequeueFinalizer(napi_ref_tracker__* finalizer);
  void DrainFinalizerQueue();

#if defined(NAPI_V8_ENABLE_LIFETIME_TRACKER) && defined(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS)
  bool should_dump_lifetime_stats(int64_t now_ms);
  bool should_dump_lifetime_string_symbol_values(int64_t now_ms);
#endif

  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context_ref;
  napi_extended_error_info last_error{};
  std::string last_error_message;
  int open_handle_scopes = 0;
  std::vector<void*> open_handle_scope_stack;
  v8::Global<v8::Value> last_exception;
  v8::Global<v8::Message> last_exception_message;
  std::string last_exception_source_line;
  std::string last_exception_thrown_at;
  v8::Global<v8::Private> wrap_private_key;
  v8::Global<v8::Private> wrap_ref_private_key;
  v8::Global<v8::Private> wrap_finalizer_private_key;
  v8::Global<v8::Private> buffer_private_key;
  v8::Global<v8::Private> type_tag_private_key;
  int32_t module_api_version = 8;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  void* edge_environment = nullptr;
  std::vector<void*> threadsafe_functions;
  std::vector<void*> async_cleanup_hooks;
  std::vector<void*> env_cleanup_hooks;
  uint64_t env_cleanup_hook_counter = 0;
  std::vector<void*> buffer_records;
  napi_ref_tracker__::RefList reflist;
  napi_ref_tracker__::RefList finalizing_reflist;
  std::unordered_set<napi_ref_tracker__*> pending_finalizers;
  bool finalization_scheduled = false;
  bool async_cleanup_hook_registered = false;
  void (*node_api_cleanup_runner)(napi_env) = nullptr;
  unofficial_napi_env_cleanup_callback env_cleanup_callback = nullptr;
  void* env_cleanup_callback_data = nullptr;
  unofficial_napi_env_destroy_callback env_destroy_callback = nullptr;
  void* env_destroy_callback_data = nullptr;
  unofficial_napi_context_token_callback context_token_assign_callback = nullptr;
  unofficial_napi_context_token_callback context_token_unassign_callback = nullptr;
  void* context_token_callback_data = nullptr;
  unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback = nullptr;
  void* enqueue_foreground_task_target = nullptr;

#if defined(NAPI_V8_ENABLE_LIFETIME_TRACKER) && defined(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS)
  int64_t lifetime_last_stats_ms_ = 0;
  int64_t lifetime_last_string_symbol_values_ms_ = 0;
#endif
};

napi_status napi_v8_set_last_error(napi_env env,
                                   napi_status status,
                                   const char* message);

napi_status napi_v8_clear_last_error(napi_env env);

napi_value napi_v8_wrap_value(napi_env env, v8::Local<v8::Value> value);
v8::Local<v8::Value> napi_v8_unwrap_value(napi_value value);
void napi_v8_finalize_buffer_records(napi_env env);

#endif  // NAPI_V8_ENV_H_
