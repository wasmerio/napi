#include "napi_quickjs_env.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

unofficial_napi_embedder_hooks g_embedder_hooks{};

napi_status Unsupported(napi_env env, const char* api) {
  return NapiQuickjsUnsupported(env, api);
}

bool IsNullish(napi_value value) {
  return value == nullptr ||
         value->kind == NapiQuickjsValueKind::kUndefined ||
         value->kind == NapiQuickjsValueKind::kNull;
}

bool IsFunction(napi_env env, napi_value value) {
  return env != nullptr &&
         value != nullptr &&
         value->has_js_value &&
         JS_IsFunction(env->context, value->js_value);
}

void ClearPendingExceptionIfAny(napi_env env) {
  if (env == nullptr) return;
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    (void)napi_get_and_clear_last_exception(env, &ignored);
  }
}

napi_value StoreHookArgument(napi_env env, JSValueConst value) {
  napi_value out = nullptr;
  if (NapiQuickjsStoreJsValue(env, value, &out) != napi_ok) {
    ClearPendingExceptionIfAny(env);
    return nullptr;
  }
  return out;
}

void CallPromiseHook(napi_env env, napi_value hook, size_t argc, napi_value* argv) {
  if (!IsFunction(env, hook)) return;
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_value ignored = nullptr;
  if (napi_call_function(env, global, hook, argc, argv, &ignored) != napi_ok) {
    ClearPendingExceptionIfAny(env);
  }
}

void CapturePromiseContextFrame(napi_env env, JSValueConst promise) {
  void* identity = NapiQuickjsJsIdentity(promise);
  if (env == nullptr || identity == nullptr) return;
  env->promise_context_frames[identity] = env->continuation_preserved_embedder_data;
}

void EnterPromiseContextFrame(napi_env env, JSValueConst promise) {
  if (env == nullptr) return;
  env->promise_context_frame_stack.push_back(env->continuation_preserved_embedder_data);

  napi_value frame = nullptr;
  void* identity = NapiQuickjsJsIdentity(promise);
  if (identity != nullptr) {
    auto it = env->promise_context_frames.find(identity);
    if (it != env->promise_context_frames.end()) {
      frame = it->second;
    }
  }
  if (frame == nullptr) {
    napi_get_undefined(env, &frame);
  }
  env->continuation_preserved_embedder_data = frame;
}

void LeavePromiseContextFrame(napi_env env, JSValueConst promise) {
  if (env == nullptr || env->promise_context_frame_stack.empty()) return;
  env->continuation_preserved_embedder_data = env->promise_context_frame_stack.back();
  env->promise_context_frame_stack.pop_back();

  void* identity = NapiQuickjsJsIdentity(promise);
  if (identity != nullptr) {
    env->promise_context_frames.erase(identity);
  }
}

JSValue QuickjsMicrotaskJob(JSContext* ctx, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_UNDEFINED;
  }
  return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
}

void QuickjsPromiseHook(JSContext* ctx,
                        JSPromiseHookType type,
                        JSValueConst promise,
                        JSValueConst parent_promise,
                        void* opaque) {
  napi_env env = static_cast<napi_env>(opaque);
  if (env == nullptr || env->context != ctx) return;

  if (type == JS_PROMISE_HOOK_INIT) {
    CapturePromiseContextFrame(env, promise);
  } else if (type == JS_PROMISE_HOOK_BEFORE) {
    EnterPromiseContextFrame(env, promise);
  }

  const size_t hook_index = static_cast<size_t>(type);
  if (hook_index >= 4) {
    if (type == JS_PROMISE_HOOK_AFTER) LeavePromiseContextFrame(env, promise);
    return;
  }
  napi_value hook = env->promise_hooks[hook_index];
  if (!IsFunction(env, hook)) {
    if (type == JS_PROMISE_HOOK_AFTER) LeavePromiseContextFrame(env, promise);
    return;
  }

  napi_value promise_arg = StoreHookArgument(env, promise);
  if (promise_arg == nullptr) {
    if (type == JS_PROMISE_HOOK_AFTER) LeavePromiseContextFrame(env, promise);
    return;
  }

  if (type == JS_PROMISE_HOOK_INIT) {
    napi_value parent_arg = StoreHookArgument(env, parent_promise);
    if (parent_arg == nullptr) return;
    napi_value argv[2] = {promise_arg, parent_arg};
    CallPromiseHook(env, hook, 2, argv);
    return;
  }

  napi_value argv[1] = {promise_arg};
  CallPromiseHook(env, hook, 1, argv);
  if (type == JS_PROMISE_HOOK_AFTER) {
    LeavePromiseContextFrame(env, promise);
  }
}

void QuickjsPromiseRejectionTracker(JSContext* ctx,
                                    JSValueConst promise,
                                    JSValueConst reason,
                                    bool is_handled,
                                    void* opaque) {
  napi_env env = static_cast<napi_env>(opaque);
  if (env == nullptr || env->context != ctx || !IsFunction(env, env->promise_reject_callback)) {
    return;
  }

  napi_value event_type = nullptr;
  napi_value promise_arg = StoreHookArgument(env, promise);
  napi_value reason_arg = StoreHookArgument(env, reason);
  if (promise_arg == nullptr || reason_arg == nullptr ||
      napi_create_int32(env, is_handled ? 1 : 0, &event_type) != napi_ok ||
      event_type == nullptr) {
    return;
  }

  napi_value argv[3] = {event_type, promise_arg, reason_arg};
  CallPromiseHook(env, env->promise_reject_callback, 3, argv);
}

napi_status GetStringValue(napi_env env, napi_value value, const char* fallback, std::string* out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  if (value == nullptr || value->kind == NapiQuickjsValueKind::kUndefined) {
    *out = fallback != nullptr ? fallback : "";
    return NapiQuickjsClearLastError(env);
  }
  if (value->kind != NapiQuickjsValueKind::kString && value->kind != NapiQuickjsValueKind::kSymbol) {
    return NapiQuickjsSetLastError(env, napi_string_expected, "String expected");
  }
  size_t length = 0;
  const char* str = JS_ToCStringLen(env->context, &length, value->js_value);
  if (str == nullptr) return NapiQuickjsSetLastError(env, napi_string_expected, "String expected");
  out->assign(str, length);
  JS_FreeCString(env->context, str);
  return NapiQuickjsClearLastError(env);
}

napi_status ReadCompileParams(napi_env env, napi_value params, std::vector<std::string>* out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  out->clear();
  if (IsNullish(params)) return NapiQuickjsClearLastError(env);

  bool is_array = false;
  if (napi_is_array(env, params, &is_array) != napi_ok || !is_array) {
    return NapiQuickjsSetLastError(env, napi_array_expected, "Compile params must be an array");
  }
  uint32_t length = 0;
  if (napi_get_array_length(env, params, &length) != napi_ok) return env->last_error.error_code;
  out->reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, params, i, &item) != napi_ok || item == nullptr) return env->last_error.error_code;
    std::string param;
    napi_status status = GetStringValue(env, item, "", &param);
    if (status != napi_ok) return status;
    out->push_back(param);
  }
  return NapiQuickjsClearLastError(env);
}

std::string BuildFunctionSource(const std::string& code, const std::vector<std::string>& params) {
  std::string source = "(function(";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i != 0) source += ",";
    source += params[i];
  }
  source += ") {\n";
  source += code;
  source += "\n})";
  return source;
}

napi_status GetHostDefinedOptionPrivateSymbol(napi_env env, napi_value* result_out) {
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = nullptr;
  if (!IsNullish(env->host_defined_option_symbol)) {
    *result_out = env->host_defined_option_symbol;
    return NapiQuickjsClearLastError(env);
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return env->last_error.error_code;

  napi_value internal_binding = nullptr;
  const char* binding_names[] = {"internalBinding", "getInternalBinding"};
  for (const char* binding_name : binding_names) {
    napi_value candidate = nullptr;
    if (napi_get_named_property(env, global, binding_name, &candidate) == napi_ok &&
        candidate != nullptr &&
        candidate->kind == NapiQuickjsValueKind::kFunction) {
      internal_binding = candidate;
      break;
    }
  }
  if (internal_binding == nullptr) {
    return NapiQuickjsClearLastError(env);
  }

  napi_value util_name = nullptr;
  if (napi_create_string_utf8(env, "util", NAPI_AUTO_LENGTH, &util_name) != napi_ok ||
      util_name == nullptr) {
    return env->last_error.error_code;
  }

  napi_value util_binding = nullptr;
  if (napi_call_function(env, global, internal_binding, 1, &util_name, &util_binding) != napi_ok ||
      util_binding == nullptr) {
    return env->last_error.error_code;
  }

  napi_value private_symbols = nullptr;
  if (napi_get_named_property(env, util_binding, "privateSymbols", &private_symbols) != napi_ok ||
      private_symbols == nullptr ||
      IsNullish(private_symbols)) {
    return NapiQuickjsClearLastError(env);
  }

  napi_value symbol = nullptr;
  if (napi_get_named_property(env, private_symbols, "host_defined_option_symbol", &symbol) != napi_ok ||
      symbol == nullptr ||
      IsNullish(symbol)) {
    return NapiQuickjsClearLastError(env);
  }

  *result_out = symbol;
  return NapiQuickjsClearLastError(env);
}

napi_status AttachHostDefinedOptionMetadata(napi_env env,
                                           napi_value target,
                                           napi_value host_defined_option_id) {
  if (env == nullptr || target == nullptr) return napi_invalid_arg;
  if (IsNullish(host_defined_option_id)) return NapiQuickjsClearLastError(env);

  napi_value host_defined_option_symbol = nullptr;
  napi_status status = GetHostDefinedOptionPrivateSymbol(env, &host_defined_option_symbol);
  if (status != napi_ok) return status;
  if (host_defined_option_symbol == nullptr || IsNullish(host_defined_option_symbol)) {
    return NapiQuickjsSetLastError(
        env,
        napi_generic_failure,
        "QuickJS contextify host-defined option symbol is unavailable");
  }

  napi_status set_status = napi_set_property(env, target, host_defined_option_symbol, host_defined_option_id);
  if (set_status != napi_ok) return set_status;

  env->host_defined_option_symbol = host_defined_option_symbol;
  if (std::find(env->host_defined_option_referrers.begin(),
                env->host_defined_option_referrers.end(),
                target) == env->host_defined_option_referrers.end()) {
    env->host_defined_option_referrers.push_back(target);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status InstallBootstrapGlobalShims(napi_env env) {
  static constexpr const char* kSource = R"js(
if (typeof globalThis.Atomics === 'undefined') {
  Object.defineProperty(globalThis, 'Atomics', {
    __proto__: null,
    configurable: true,
    writable: true,
    value: Object.freeze(Object.create(null)),
  });
}
if (typeof globalThis.Symbol === 'function') {
  const ensureSymbol = (name, description) => {
    if (typeof Symbol[name] === 'undefined') {
      Object.defineProperty(Symbol, name, {
        __proto__: null,
        configurable: false,
        enumerable: false,
        writable: false,
        value: Symbol(description),
      });
    }
  };
  ensureSymbol('dispose', 'Symbol.dispose');
  ensureSymbol('asyncDispose', 'Symbol.asyncDispose');
}
)js";

  JSValue value = JS_Eval(env->context,
                          kSource,
                          std::strlen(kSource),
                          "<quickjs-bootstrap-globals>",
                          JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(value)) {
    JSValue exception = JS_GetException(env->context);
    JS_FreeValue(env->context, exception);
    return NapiQuickjsSetLastError(env,
                                   napi_generic_failure,
                                   "Failed to install QuickJS bootstrap global shims");
  }
  JS_FreeValue(env->context, value);
  return NapiQuickjsClearLastError(env);
}

napi_status CreateEnvImpl(int32_t module_api_version, napi_env* env_out, void** scope_out) {
  if (env_out == nullptr || scope_out == nullptr) return napi_invalid_arg;
  auto* env = new (std::nothrow) napi_env__();
  if (env == nullptr) return napi_generic_failure;
  env->module_api_version = module_api_version;
  env->runtime = JS_NewRuntime();
  if (env->runtime == nullptr) {
    delete env;
    return napi_generic_failure;
  }
  env->context = JS_NewContext(env->runtime);
  if (env->context == nullptr) {
    JS_FreeRuntime(env->runtime);
    delete env;
    return napi_generic_failure;
  }
  JS_SetContextOpaque(env->context, env);
  JS_SetPromiseHook(env->runtime, QuickjsPromiseHook, env);
  NapiQuickjsClearLastError(env);
  if (InstallBootstrapGlobalShims(env) != napi_ok) {
    NapiQuickjsReleaseEnv(env);
    return napi_generic_failure;
  }

  auto* scope = new (std::nothrow) NapiQuickjsEnvScope();
  if (scope == nullptr) {
    NapiQuickjsReleaseEnv(env);
    return napi_generic_failure;
  }
  scope->env = env;
  *env_out = env;
  *scope_out = scope;
  return napi_ok;
}

}  // namespace

EXTERN_C_START

napi_status NAPI_CDECL unofficial_napi_create_env(
    int32_t module_api_version, napi_env* env_out, void** scope_out) {
  return CreateEnvImpl(module_api_version, env_out, scope_out);
}

napi_status NAPI_CDECL unofficial_napi_create_env_with_options(
    int32_t module_api_version,
    const unofficial_napi_env_create_options* options,
    napi_env* env_out,
    void** scope_out) {
  (void)options;
  return CreateEnvImpl(module_api_version, env_out, scope_out);
}

napi_status NAPI_CDECL unofficial_napi_set_embedder_hooks(
    const unofficial_napi_embedder_hooks* hooks) {
  g_embedder_hooks = hooks != nullptr ? *hooks : unofficial_napi_embedder_hooks{};
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_set_edge_environment(napi_env env, void* environment) {
  if (env == nullptr) return napi_invalid_arg;
  env->edge_environment = environment;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_env_cleanup_callback(
    napi_env env, unofficial_napi_env_cleanup_callback callback, void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->cleanup_callback = callback;
  env->cleanup_data = data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_env_destroy_callback(
    napi_env env, unofficial_napi_env_destroy_callback callback, void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->destroy_callback = callback;
  env->destroy_data = data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_context_token_callbacks(
    napi_env env,
    unofficial_napi_context_token_callback assign_callback,
    unofficial_napi_context_token_callback unassign_callback,
    void* data) {
  if (env == nullptr) return napi_invalid_arg;
  env->context_assign_callback = assign_callback;
  env->context_unassign_callback = unassign_callback;
  env->context_token_data = data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_release_env(void* scope_ptr) {
  auto* scope = static_cast<NapiQuickjsEnvScope*>(scope_ptr);
  if (scope == nullptr || scope->env == nullptr) return napi_invalid_arg;
  napi_env env = scope->env;
  delete scope;
  return NapiQuickjsReleaseEnv(env);
}

napi_status NAPI_CDECL unofficial_napi_release_env_with_loop(void* scope_ptr, struct uv_loop_s* loop) {
  (void)loop;
  return unofficial_napi_release_env(scope_ptr);
}

napi_status NAPI_CDECL unofficial_napi_low_memory_notification(napi_env env) {
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_set_flags_from_string(const char* flags, size_t length) {
  (void)flags;
  (void)length;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_set_prepare_stack_trace_callback(napi_env env, napi_value callback) {
  (void)callback;
  return Unsupported(env, "QuickJS provider does not support prepareStackTrace callbacks yet");
}

napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env) {
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env) {
  return env != nullptr ? NapiQuickjsDrainPromiseJobs(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_terminate_execution(napi_env env) {
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_cancel_terminate_execution(napi_env env) {
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_request_interrupt(
    napi_env env, unofficial_napi_interrupt_callback callback, void* data) {
  if (env == nullptr || callback == nullptr) return napi_invalid_arg;
  callback(env, data);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_enqueue_foreground_task_callback(
    napi_env env, unofficial_napi_enqueue_foreground_task_callback callback, void* target) {
  if (env == nullptr) return napi_invalid_arg;
  env->foreground_task_callback = callback;
  env->foreground_task_target = target;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback) {
  if (env == nullptr || env->context == nullptr || callback == nullptr) return napi_invalid_arg;
  if (!IsFunction(env, callback)) {
    return NapiQuickjsSetLastError(env, napi_function_expected, "Function expected");
  }
  JSValueConst job_args[1] = {callback->js_value};
  if (JS_EnqueueJob(env->context, QuickjsMicrotaskJob, 1, job_args) < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env, napi_value callback) {
  if (env == nullptr || env->runtime == nullptr) return napi_invalid_arg;
  env->promise_reject_callback = IsFunction(env, callback) ? callback : nullptr;
  JS_SetHostPromiseRejectionTracker(env->runtime,
                                    env->promise_reject_callback != nullptr
                                        ? QuickjsPromiseRejectionTracker
                                        : nullptr,
                                    env);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_promise_hooks(
    napi_env env, napi_value init, napi_value before, napi_value after, napi_value resolve) {
  if (env == nullptr || env->runtime == nullptr) return napi_invalid_arg;
  napi_value hooks[4] = {init, before, after, resolve};
  bool any_hook = false;
  for (size_t i = 0; i < 4; ++i) {
    env->promise_hooks[i] = IsFunction(env, hooks[i]) ? hooks[i] : nullptr;
    any_hook = any_hook || env->promise_hooks[i] != nullptr;
  }
  (void)any_hook;
  JS_SetPromiseHook(env->runtime, QuickjsPromiseHook, env);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_fatal_error_callbacks(
    napi_env env, unofficial_napi_fatal_error_callback fatal_callback, unofficial_napi_oom_error_callback oom_callback) {
  (void)fatal_callback;
  (void)oom_callback;
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_set_near_heap_limit_callback(
    napi_env env, unofficial_napi_near_heap_limit_callback callback, void* data) {
  (void)callback;
  (void)data;
  return Unsupported(env, "QuickJS provider does not support heap limit callbacks yet");
}

napi_status NAPI_CDECL unofficial_napi_remove_near_heap_limit_callback(napi_env env, size_t heap_limit) {
  (void)heap_limit;
  return Unsupported(env, "QuickJS provider does not support heap limit callbacks yet");
}

napi_status NAPI_CDECL unofficial_napi_set_stack_limit(napi_env env, void* stack_limit) {
  (void)stack_limit;
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_get_promise_details(
    napi_env env, napi_value promise, int32_t* state_out, napi_value* result_out, bool* has_result_out) {
  (void)promise;
  if (state_out != nullptr) *state_out = 0;
  if (result_out != nullptr) napi_get_undefined(env, result_out);
  if (has_result_out != nullptr) *has_result_out = false;
  return Unsupported(env, "QuickJS provider does not support promise details yet");
}

napi_status NAPI_CDECL unofficial_napi_get_error_source_positions(
    napi_env env, napi_value error, unofficial_napi_error_source_positions* out) {
  (void)error;
  if (out != nullptr) *out = unofficial_napi_error_source_positions{};
  return Unsupported(env, "QuickJS provider does not support error source positions yet");
}

napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(napi_env env, napi_value error) {
  (void)error;
  return Unsupported(env, "QuickJS provider does not support preserved error formatting yet");
}

napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(napi_env env, bool enabled) {
  (void)enabled;
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(napi_env env, napi_value callback) {
  (void)callback;
  return Unsupported(env, "QuickJS provider does not support source map callbacks yet");
}

napi_status NAPI_CDECL unofficial_napi_get_error_source_line_for_stderr(
    napi_env env, napi_value error, napi_value* result_out) {
  (void)error;
  if (result_out != nullptr) napi_get_undefined(env, result_out);
  return Unsupported(env, "QuickJS provider does not support source lines yet");
}

napi_status NAPI_CDECL unofficial_napi_get_error_thrown_at(
    napi_env env, napi_value error, napi_value* result_out) {
  (void)error;
  if (result_out != nullptr) napi_get_undefined(env, result_out);
  return Unsupported(env, "QuickJS provider does not support thrown-at metadata yet");
}

napi_status NAPI_CDECL unofficial_napi_take_preserved_error_formatting(
    napi_env env, napi_value error, napi_value* source_line_out, napi_value* thrown_at_out) {
  (void)error;
  if (source_line_out != nullptr) napi_get_undefined(env, source_line_out);
  if (thrown_at_out != nullptr) napi_get_undefined(env, thrown_at_out);
  return Unsupported(env, "QuickJS provider does not support preserved error formatting yet");
}

napi_status NAPI_CDECL unofficial_napi_mark_promise_as_handled(napi_env env, napi_value promise) {
  (void)promise;
  return Unsupported(env, "QuickJS provider does not support promise handling metadata yet");
}

napi_status NAPI_CDECL unofficial_napi_get_proxy_details(
    napi_env env, napi_value proxy, napi_value* target_out, napi_value* handler_out) {
  (void)proxy;
  if (target_out != nullptr) napi_get_undefined(env, target_out);
  if (handler_out != nullptr) napi_get_undefined(env, handler_out);
  return Unsupported(env, "QuickJS provider does not support proxy details yet");
}

napi_status NAPI_CDECL unofficial_napi_preview_entries(
    napi_env env, napi_value value, napi_value* entries_out, bool* is_key_value_out) {
  (void)value;
  if (entries_out != nullptr) napi_create_array(env, entries_out);
  if (is_key_value_out != nullptr) *is_key_value_out = false;
  return Unsupported(env, "QuickJS provider does not support entry previews yet");
}

napi_status NAPI_CDECL unofficial_napi_get_call_sites(
    napi_env env, uint32_t frames, napi_value* callsites_out) {
  (void)frames;
  if (callsites_out != nullptr) napi_create_array(env, callsites_out);
  return Unsupported(env, "QuickJS provider does not support call sites yet");
}

napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(
    napi_env env, napi_value value, bool* result_out) {
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = value != nullptr && value->arraybuffer != nullptr;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_get_constructor_name(
    napi_env env, napi_value value, napi_value* name_out) {
  (void)value;
  if (name_out == nullptr) return napi_invalid_arg;
  return napi_create_string_utf8(env, "Object", NAPI_AUTO_LENGTH, name_out);
}

napi_status NAPI_CDECL unofficial_napi_get_own_non_index_properties(
    napi_env env, napi_value value, uint32_t filter_bits, napi_value* result_out) {
  (void)filter_bits;
  return napi_get_property_names(env, value, result_out);
}

napi_status NAPI_CDECL unofficial_napi_create_private_symbol(
    napi_env env, const char* utf8description, size_t length, napi_value* result_out) {
  napi_value description = nullptr;
  napi_status status = napi_create_string_utf8(env, utf8description, length, &description);
  if (status != napi_ok) return status;
  return napi_create_symbol(env, description, result_out);
}

napi_status NAPI_CDECL unofficial_napi_structured_clone(
    napi_env env, napi_value value, napi_value transfer_list_or_null, napi_value* result_out) {
  (void)transfer_list_or_null;
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = value;
  return Unsupported(env, "QuickJS provider does not support structured clone yet");
}

napi_status NAPI_CDECL unofficial_napi_serialize_value(napi_env env, napi_value value, void** payload_out) {
  if (env == nullptr || payload_out == nullptr) return napi_invalid_arg;
  *payload_out = value;
  return Unsupported(env, "QuickJS provider does not support serialization yet");
}

napi_status NAPI_CDECL unofficial_napi_deserialize_value(napi_env env, void* payload, napi_value* result_out) {
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = static_cast<napi_value>(payload);
  return Unsupported(env, "QuickJS provider does not support deserialization yet");
}

void NAPI_CDECL unofficial_napi_release_serialized_value(void* payload) {
  (void)payload;
}

napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
    napi_env env, double* heap_total_out, double* heap_used_out, double* external_out, double* array_buffers_out) {
  if (env == nullptr) return napi_invalid_arg;
  if (heap_total_out != nullptr) *heap_total_out = 0;
  if (heap_used_out != nullptr) *heap_used_out = 0;
  if (external_out != nullptr) *external_out = 0;
  if (array_buffers_out != nullptr) *array_buffers_out = 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_get_hash_seed(napi_env env, uint64_t* hash_seed_out) {
  if (env == nullptr || hash_seed_out == nullptr) return napi_invalid_arg;
  *hash_seed_out = 1;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_statistics(
    napi_env env, unofficial_napi_heap_statistics* stats_out) {
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  *stats_out = unofficial_napi_heap_statistics{};
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_space_count(napi_env env, uint32_t* count_out) {
  if (env == nullptr || count_out == nullptr) return napi_invalid_arg;
  *count_out = 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_get_heap_space_statistics(
    napi_env env, uint32_t space_index, unofficial_napi_heap_space_statistics* stats_out) {
  (void)space_index;
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  *stats_out = unofficial_napi_heap_space_statistics{};
  return Unsupported(env, "QuickJS provider does not support heap space statistics yet");
}

napi_status NAPI_CDECL unofficial_napi_get_heap_code_statistics(
    napi_env env, unofficial_napi_heap_code_statistics* stats_out) {
  if (env == nullptr || stats_out == nullptr) return napi_invalid_arg;
  *stats_out = unofficial_napi_heap_code_statistics{};
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_start_cpu_profile(
    napi_env env, unofficial_napi_cpu_profile_start_result* result_out, uint32_t* profile_id_out) {
  if (result_out != nullptr) *result_out = unofficial_napi_cpu_profile_start_too_many;
  if (profile_id_out != nullptr) *profile_id_out = 0;
  return Unsupported(env, "QuickJS provider does not support CPU profiles yet");
}

napi_status NAPI_CDECL unofficial_napi_stop_cpu_profile(
    napi_env env, uint32_t profile_id, bool* found_out, char** json_out, size_t* json_len_out) {
  (void)profile_id;
  if (found_out != nullptr) *found_out = false;
  if (json_out != nullptr) *json_out = nullptr;
  if (json_len_out != nullptr) *json_len_out = 0;
  return Unsupported(env, "QuickJS provider does not support CPU profiles yet");
}

napi_status NAPI_CDECL unofficial_napi_start_heap_profile(napi_env env, bool* started_out) {
  if (started_out != nullptr) *started_out = false;
  return Unsupported(env, "QuickJS provider does not support heap profiles yet");
}

napi_status NAPI_CDECL unofficial_napi_stop_heap_profile(
    napi_env env, bool* found_out, char** json_out, size_t* json_len_out) {
  if (found_out != nullptr) *found_out = false;
  if (json_out != nullptr) *json_out = nullptr;
  if (json_len_out != nullptr) *json_len_out = 0;
  return Unsupported(env, "QuickJS provider does not support heap profiles yet");
}

napi_status NAPI_CDECL unofficial_napi_take_heap_snapshot(
    napi_env env, const unofficial_napi_heap_snapshot_options* options, char** json_out, size_t* json_len_out) {
  (void)options;
  if (json_out != nullptr) *json_out = nullptr;
  if (json_len_out != nullptr) *json_len_out = 0;
  return Unsupported(env, "QuickJS provider does not support heap snapshots yet");
}

void NAPI_CDECL unofficial_napi_free_buffer(void* data) {
  std::free(data);
}

napi_status NAPI_CDECL unofficial_napi_get_continuation_preserved_embedder_data(
    napi_env env, napi_value* result_out) {
  if (env == nullptr || result_out == nullptr) return napi_invalid_arg;
  if (env->continuation_preserved_embedder_data == nullptr) {
    return napi_get_undefined(env, result_out);
  }
  *result_out = env->continuation_preserved_embedder_data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
    napi_env env, napi_value value) {
  if (env == nullptr) return napi_invalid_arg;
  env->continuation_preserved_embedder_data = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env) {
  return env != nullptr ? NapiQuickjsClearLastError(env) : napi_invalid_arg;
}

napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env, napi_value* result_out) {
  if (result_out != nullptr) napi_create_object(env, result_out);
  return Unsupported(env, "QuickJS provider does not support serdes binding yet");
}

#define QUICKJS_UNSUPPORTED_CONTEXTIFY(name, ...) \
  napi_status NAPI_CDECL name(__VA_ARGS__) { return Unsupported(env, "QuickJS provider does not support " #name " yet"); }

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_contextify_make_context,
    napi_env env, napi_value sandbox_or_symbol, napi_value name, napi_value origin_or_undefined,
    bool allow_code_gen_strings, bool allow_code_gen_wasm, bool own_microtask_queue,
    napi_value host_defined_option_id, napi_value* result_out)

napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
    napi_env env, napi_value sandbox_or_null, napi_value source, napi_value filename,
    int32_t line_offset, int32_t column_offset, int64_t timeout, bool display_errors,
    bool break_on_sigint, bool break_on_first_line, napi_value host_defined_option_id,
    napi_value* result_out) {
  (void)line_offset;
  (void)column_offset;
  (void)timeout;
  (void)display_errors;
  (void)break_on_sigint;
  (void)break_on_first_line;
  (void)host_defined_option_id;
  if (env == nullptr || source == nullptr) return napi_invalid_arg;
  if (!IsNullish(sandbox_or_null)) {
    return Unsupported(env, "QuickJS provider does not support alternate contextify run-script contexts yet");
  }
  std::string source_text;
  napi_status status = GetStringValue(env, source, "", &source_text);
  if (status != napi_ok) return status;
  std::string filename_text;
  status = GetStringValue(env, filename, "[eval]", &filename_text);
  if (status != napi_ok) return status;
  JSValue value = JS_Eval(env->context,
                          source_text.c_str(),
                          source_text.size(),
                          filename_text.c_str(),
                          JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(value)) return NapiQuickjsStorePendingException(env);
  return result_out != nullptr ? NapiQuickjsStoreOwnedJsValue(env, value, result_out)
                               : (JS_FreeValue(env->context, value), NapiQuickjsClearLastError(env));
}

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_contextify_dispose_context,
    napi_env env, napi_value sandbox_or_context_global)

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
    napi_env env, napi_value code, napi_value filename, int32_t line_offset, int32_t column_offset,
    napi_value cached_data_or_undefined, bool produce_cached_data, napi_value parsing_context_or_undefined,
    napi_value context_extensions_or_undefined, napi_value params_or_undefined,
    napi_value host_defined_option_id, napi_value* result_out) {
  (void)line_offset;
  (void)column_offset;
  (void)host_defined_option_id;
  if (env == nullptr || env->context == nullptr || code == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }
  if (!IsNullish(cached_data_or_undefined)) {
    return Unsupported(env, "QuickJS provider does not support contextify cached data yet");
  }
  if (produce_cached_data) {
    return Unsupported(env, "QuickJS provider does not support producing contextify cached data yet");
  }
  if (!IsNullish(parsing_context_or_undefined)) {
    return Unsupported(env, "QuickJS provider does not support alternate contextify parsing contexts yet");
  }
  if (!IsNullish(context_extensions_or_undefined)) {
    return Unsupported(env, "QuickJS provider does not support contextify context extensions yet");
  }

  std::string source_text;
  napi_status status = GetStringValue(env, code, "", &source_text);
  if (status != napi_ok) return status;

  std::string filename_text;
  status = GetStringValue(env, filename, "[eval]", &filename_text);
  if (status != napi_ok) return status;

  std::vector<std::string> params;
  status = ReadCompileParams(env, params_or_undefined, &params);
  if (status != napi_ok) return status;

  const std::string wrapped_source = BuildFunctionSource(source_text, params);
  JSValue fn = JS_Eval(env->context,
                       wrapped_source.c_str(),
                       wrapped_source.size(),
                       filename_text.c_str(),
                       JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) {
    JSValue exception = JS_GetException(env->context);
    napi_value pending = nullptr;
    status = NapiQuickjsStoreJsValue(env, exception, &pending);
    JS_FreeValue(env->context, exception);
    if (status == napi_ok) env->pending_exception = pending;
    return napi_pending_exception;
  }
  if (!JS_IsFunction(env->context, fn)) {
    JS_FreeValue(env->context, fn);
    return NapiQuickjsSetLastError(env, napi_generic_failure, "QuickJS compile did not produce a function");
  }

  napi_value fn_value = nullptr;
  status = NapiQuickjsStoreOwnedJsValue(env, fn, &fn_value);
  if (status != napi_ok) return status;
  status = AttachHostDefinedOptionMetadata(env, fn_value, host_defined_option_id);
  if (status != napi_ok) return status;

  napi_value out = nullptr;
  status = napi_create_object(env, &out);
  if (status != napi_ok || out == nullptr) return status != napi_ok ? status : napi_generic_failure;
  if (napi_set_named_property(env, out, "function", fn_value) != napi_ok) return env->last_error.error_code;

  napi_value source_url = nullptr;
  if (napi_create_string_utf8(env, filename_text.c_str(), filename_text.size(), &source_url) == napi_ok &&
      source_url != nullptr) {
    (void)napi_set_named_property(env, out, "sourceURL", source_url);
  }
  napi_value undefined = nullptr;
  if (napi_get_undefined(env, &undefined) == napi_ok && undefined != nullptr) {
    (void)napi_set_named_property(env, out, "sourceMapURL", undefined);
  }

  *result_out = out;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
    napi_env env, napi_value code, napi_value filename, napi_value resource_name_or_undefined,
    bool cjs_var_in_scope, bool* result_out) {
  (void)code;
  (void)filename;
  (void)resource_name_or_undefined;
  (void)cjs_var_in_scope;
  if (result_out != nullptr) *result_out = false;
  return Unsupported(env, "QuickJS provider does not support module syntax detection yet");
}

napi_status NAPI_CDECL unofficial_napi_contextify_create_cached_data(
    napi_env env, napi_value code, napi_value filename, int32_t line_offset, int32_t column_offset,
    napi_value host_defined_option_id, napi_value* cached_data_buffer_out) {
  (void)line_offset;
  (void)column_offset;
  (void)host_defined_option_id;
  if (env == nullptr || env->context == nullptr || code == nullptr || cached_data_buffer_out == nullptr) {
    return napi_invalid_arg;
  }

  std::string source_text;
  napi_status status = GetStringValue(env, code, "", &source_text);
  if (status != napi_ok) return status;
  std::string filename_text;
  status = GetStringValue(env, filename, "[eval]", &filename_text);
  if (status != napi_ok) return status;

  JSValue compiled = JS_Eval(env->context,
                             source_text.c_str(),
                             source_text.size(),
                             filename_text.c_str(),
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) return NapiQuickjsStorePendingException(env);
  JS_FreeValue(env->context, compiled);

  void* data = nullptr;
  return napi_create_buffer(env, 0, &data, cached_data_buffer_out);
}

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_create_source_text,
    napi_env env, napi_value wrapper, napi_value url, napi_value context_or_undefined,
    napi_value source, int32_t line_offset, int32_t column_offset, napi_value cached_data_or_id,
    void** handle_out)

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_create_synthetic,
    napi_env env, napi_value wrapper, napi_value url, napi_value context_or_undefined,
    napi_value export_names, napi_value synthetic_eval_steps, void** handle_out)

napi_status NAPI_CDECL unofficial_napi_module_wrap_destroy(napi_env env, void* handle) {
  (void)handle;
  return Unsupported(env, "QuickJS provider does not support module wraps yet");
}

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_get_module_requests,
    napi_env env, void* handle, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_link,
    napi_env env, void* handle, size_t count, void* const* linked_handles)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_instantiate,
    napi_env env, void* handle)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_evaluate,
    napi_env env, void* handle, int64_t timeout, bool break_on_sigint, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_evaluate_sync,
    napi_env env, void* handle, napi_value filename, napi_value parent_filename, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_get_namespace,
    napi_env env, void* handle, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_get_status,
    napi_env env, void* handle, int32_t* status_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_get_error,
    napi_env env, void* handle, napi_value* result_out)

napi_status NAPI_CDECL unofficial_napi_module_wrap_has_top_level_await(
    napi_env env, void* handle, bool* result_out) {
  (void)handle;
  if (result_out != nullptr) *result_out = false;
  return Unsupported(env, "QuickJS provider does not support module wraps yet");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_has_async_graph(
    napi_env env, void* handle, bool* result_out) {
  (void)handle;
  if (result_out != nullptr) *result_out = false;
  return Unsupported(env, "QuickJS provider does not support module wraps yet");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_check_unsettled_top_level_await(
    napi_env env, napi_value module_wrap, bool warnings, bool* settled_out) {
  (void)module_wrap;
  (void)warnings;
  if (settled_out != nullptr) *settled_out = true;
  return Unsupported(env, "QuickJS provider does not support module wraps yet");
}

QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_set_export,
    napi_env env, void* handle, napi_value export_name, napi_value export_value)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_set_module_source_object,
    napi_env env, void* handle, napi_value source_object)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_get_module_source_object,
    napi_env env, void* handle, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_create_cached_data,
    napi_env env, void* handle, napi_value* result_out)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_set_import_module_dynamically_callback,
    napi_env env, napi_value callback)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_set_initialize_import_meta_object_callback,
    napi_env env, napi_value callback)
QUICKJS_UNSUPPORTED_CONTEXTIFY(unofficial_napi_module_wrap_create_required_module_facade,
    napi_env env, void* handle, napi_value* result_out)

#undef QUICKJS_UNSUPPORTED_CONTEXTIFY

EXTERN_C_END
