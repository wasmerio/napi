#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "node_api.h"
#include "quickjs.h"
#include "unofficial_napi.h"

enum class NapiQuickjsValueKind {
  kUndefined,
  kNull,
  kBoolean,
  kNumber,
  kString,
  kObject,
  kArray,
  kFunction,
  kExternal,
  kError,
  kBuffer,
  kArrayBuffer,
  kTypedArray,
  kDataView,
  kPromise,
  kBigInt,
  kDate,
  kSymbol,
};

struct napi_value__ {
  explicit napi_value__(NapiQuickjsValueKind kind_in) : kind(kind_in) {}

  NapiQuickjsValueKind kind;
  JSValue js_value = JS_UNDEFINED;
  bool has_js_value = false;
  std::string debug_name;
  napi_callback callback = nullptr;
  void* callback_data = nullptr;
  bool is_class_constructor = false;
  void* external_data = nullptr;
  node_api_basic_finalize finalize_cb = nullptr;
  void* finalize_hint = nullptr;
  std::vector<uint8_t> bytes;
  napi_typedarray_type typedarray_type = napi_uint8_array;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  size_t byte_length = 0;
};

struct napi_ref__ {
  napi_env env = nullptr;
  napi_value value = nullptr;
  JSValue js_value = JS_UNDEFINED;
  bool has_js_value = false;
  uint32_t refcount = 0;
};

struct napi_handle_scope__ {
  napi_env env = nullptr;
};

struct napi_escapable_handle_scope__ {
  napi_env env = nullptr;
  bool escaped = false;
};

struct napi_callback_info__ {
  napi_env env = nullptr;
  napi_value this_arg = nullptr;
  napi_value new_target = nullptr;
  std::vector<napi_value> args;
  void* data = nullptr;
};

struct napi_deferred__ {
  napi_env env = nullptr;
  napi_value promise = nullptr;
  napi_value resolve = nullptr;
  napi_value reject = nullptr;
};

struct napi_async_cleanup_hook_handle__ {
  napi_env env = nullptr;
  napi_async_cleanup_hook hook = nullptr;
  void* arg = nullptr;
  bool removed = false;
};

struct NapiQuickjsNativeCallback {
  napi_callback callback = nullptr;
  void* data = nullptr;
  napi_value function_value = nullptr;
};

struct napi_env__ {
  int32_t module_api_version = 8;
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
  napi_extended_error_info last_error{};
  std::string last_error_message;
  napi_value pending_exception = nullptr;
  napi_value continuation_preserved_embedder_data = nullptr;
  napi_value host_defined_option_symbol = nullptr;
  napi_value promise_hooks[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_value promise_reject_callback = nullptr;
  std::unordered_map<void*, napi_value> promise_context_frames;
  std::vector<napi_value> promise_context_frame_stack;
  std::vector<napi_value> host_defined_option_referrers;
  void* instance_data = nullptr;
  napi_finalize instance_data_finalize_cb = nullptr;
  void* instance_data_finalize_hint = nullptr;
  void* edge_environment = nullptr;
  unofficial_napi_env_cleanup_callback cleanup_callback = nullptr;
  void* cleanup_data = nullptr;
  unofficial_napi_env_destroy_callback destroy_callback = nullptr;
  void* destroy_data = nullptr;
  unofficial_napi_context_token_callback context_assign_callback = nullptr;
  unofficial_napi_context_token_callback context_unassign_callback = nullptr;
  void* context_token_data = nullptr;
  unofficial_napi_enqueue_foreground_task_callback foreground_task_callback = nullptr;
  void* foreground_task_target = nullptr;
  napi_value global_object = nullptr;
  napi_value object_constructor = nullptr;
  napi_value object_prototype = nullptr;
  std::vector<napi_value> values;
  std::unordered_map<void*, napi_value> js_to_napi_values;
  std::vector<napi_ref> refs;
  std::vector<NapiQuickjsNativeCallback> native_callbacks;
  std::vector<napi_cleanup_hook> cleanup_hooks;
  std::vector<void*> cleanup_hook_args;
  std::vector<napi_async_cleanup_hook_handle> async_cleanup_hooks;
};

struct NapiQuickjsEnvScope {
  napi_env env = nullptr;
};

napi_status NapiQuickjsSetLastError(napi_env env,
                                    napi_status status,
                                    const char* message = nullptr);
napi_status NapiQuickjsClearLastError(napi_env env);
bool NapiQuickjsIsEnv(napi_env env);
napi_value NapiQuickjsMakeValue(napi_env env, NapiQuickjsValueKind kind);
napi_status NapiQuickjsValueToPropertyKey(napi_env env,
                                          napi_value value,
                                          std::string* key);
napi_valuetype NapiQuickjsTypeOf(napi_value value);
napi_status NapiQuickjsUnsupported(napi_env env, const char* message);
napi_status NapiQuickjsInvalidArg(napi_env env, const char* message = nullptr);
napi_status NapiQuickjsReleaseEnv(napi_env env);
napi_status NapiQuickjsWrapOwnedValue(napi_env env,
                                      NapiQuickjsValueKind kind,
                                      JSValue js_value,
                                      napi_value* result);
napi_status NapiQuickjsStoreJsValue(napi_env env, JSValueConst js_value, napi_value* result);
napi_status NapiQuickjsStoreOwnedJsValue(napi_env env,
                                         JSValue js_value,
                                         napi_value* result);
JSValue NapiQuickjsValueToJsValue(napi_env env, napi_value value);
void* NapiQuickjsJsIdentity(JSValueConst js_value);
void NapiQuickjsRememberJsValue(napi_env env, napi_value value);
napi_value NapiQuickjsFindNapiValue(napi_env env, JSValueConst js_value);
napi_status NapiQuickjsStorePendingException(napi_env env);
JSValue NapiQuickjsTakePendingException(napi_env env);
JSValue NapiQuickjsCreateNativeFunction(napi_env env, napi_value function_value);
napi_status NapiQuickjsDrainPromiseJobs(napi_env env);

#endif  // NAPI_QUICKJS_ENV_H_
