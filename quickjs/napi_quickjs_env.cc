#include "napi_quickjs_env.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr const char* kGenericFailure = "QuickJS N-API provider does not implement this API yet";

NapiQuickjsValueKind KindForJsValue(napi_env env, JSValueConst value) {
  if (JS_IsUndefined(value)) return NapiQuickjsValueKind::kUndefined;
  if (JS_IsNull(value)) return NapiQuickjsValueKind::kNull;
  if (JS_IsBool(value)) return NapiQuickjsValueKind::kBoolean;
  if (JS_IsNumber(value)) return NapiQuickjsValueKind::kNumber;
  if (JS_IsString(value)) return NapiQuickjsValueKind::kString;
  if (JS_IsSymbol(value)) return NapiQuickjsValueKind::kSymbol;
  if (JS_IsFunction(env->context, value)) return NapiQuickjsValueKind::kFunction;
  if (JS_IsArray(value)) return NapiQuickjsValueKind::kArray;
  if (JS_IsArrayBuffer(value)) return NapiQuickjsValueKind::kArrayBuffer;
  if (JS_GetTypedArrayType(value) >= 0) return NapiQuickjsValueKind::kTypedArray;
  return NapiQuickjsValueKind::kObject;
}

JSValue NapiQuickjsNativeFunctionBridge(JSContext* ctx,
                                        JSValueConst this_val,
                                        int argc,
                                        JSValueConst* argv,
                                        int magic,
                                        JSValueConst* func_data) {
  (void)magic;
  napi_env env = static_cast<napi_env>(JS_GetContextOpaque(ctx));
  uint32_t callback_id = 0;
  if (env == nullptr || func_data == nullptr || JS_ToUint32(ctx, &callback_id, func_data[0]) < 0 ||
      callback_id >= env->native_callbacks.size() ||
      env->native_callbacks[callback_id].callback == nullptr) {
    return JS_ThrowInternalError(ctx, "QuickJS native callback has no N-API environment");
  }
  const NapiQuickjsNativeCallback& callback = env->native_callbacks[callback_id];
  env->pending_exception = nullptr;

  JSValue constructed_this = JS_UNDEFINED;
  JSValueConst effective_this = this_val;
  napi_value function_value = callback.function_value;
  const bool is_constructor_call =
      function_value != nullptr &&
      function_value->is_class_constructor &&
      function_value->has_js_value &&
      NapiQuickjsJsIdentity(this_val) == NapiQuickjsJsIdentity(function_value->js_value);
  if (is_constructor_call) {
    JSValue prototype = JS_GetPropertyStr(ctx, this_val, "prototype");
    if (JS_IsException(prototype)) return prototype;
    constructed_this = JS_IsObject(prototype) ? JS_NewObjectProto(ctx, prototype) : JS_NewObject(ctx);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(constructed_this)) return constructed_this;
    effective_this = constructed_this;
  }

  napi_value recv = nullptr;
  if (NapiQuickjsStoreJsValue(env, effective_this, &recv) != napi_ok) {
    JS_FreeValue(ctx, constructed_this);
    return JS_ThrowInternalError(ctx, "Failed to bridge QuickJS receiver");
  }

  std::vector<napi_value> napi_args;
  napi_args.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
  for (int i = 0; i < argc; ++i) {
    napi_value arg = nullptr;
    if (NapiQuickjsStoreJsValue(env, argv[i], &arg) != napi_ok) {
      JS_FreeValue(ctx, constructed_this);
      return JS_ThrowInternalError(ctx, "Failed to bridge QuickJS argument");
    }
    napi_args.push_back(arg);
  }

  auto* info = new (std::nothrow) napi_callback_info__();
  if (info == nullptr) {
    JS_FreeValue(ctx, constructed_this);
    return JS_ThrowInternalError(ctx, "Failed to allocate N-API callback info");
  }
  info->env = env;
  info->this_arg = recv;
  info->new_target = is_constructor_call ? function_value : nullptr;
  info->args = std::move(napi_args);
  info->data = callback.data;

  napi_value result = callback.callback(env, info);
  delete info;

  if (env->pending_exception != nullptr) {
    JS_FreeValue(ctx, constructed_this);
    return NapiQuickjsTakePendingException(env);
  }
  JSValue js_result = JS_UNDEFINED;
  if (result != nullptr && (!is_constructor_call || (result->has_js_value && JS_IsObject(result->js_value)))) {
    js_result = NapiQuickjsValueToJsValue(env, result);
  } else if (is_constructor_call) {
    js_result = NapiQuickjsValueToJsValue(env, recv);
  }
  JS_FreeValue(ctx, constructed_this);
  return js_result;
}

}  // namespace

napi_status NapiQuickjsSetLastError(napi_env env,
                                    napi_status status,
                                    const char* message) {
  if (env != nullptr) {
    env->last_error_message = message != nullptr ? message : "";
    env->last_error.error_message =
        env->last_error_message.empty() ? nullptr : env->last_error_message.c_str();
    env->last_error.engine_reserved = nullptr;
    env->last_error.engine_error_code = 0;
    env->last_error.error_code = status;
  }
  return status;
}

napi_status NapiQuickjsClearLastError(napi_env env) {
  return NapiQuickjsSetLastError(env, napi_ok, nullptr);
}

bool NapiQuickjsIsEnv(napi_env env) {
  return env != nullptr;
}

napi_value NapiQuickjsMakeValue(napi_env env, NapiQuickjsValueKind kind) {
  if (env == nullptr) return nullptr;
  auto* value = new (std::nothrow) napi_value__(kind);
  if (value == nullptr) {
    (void)NapiQuickjsSetLastError(env, napi_generic_failure, "Failed to allocate napi_value");
    return nullptr;
  }
  env->values.push_back(value);
  return value;
}

napi_status NapiQuickjsWrapOwnedValue(napi_env env,
                                      NapiQuickjsValueKind kind,
                                      JSValue js_value,
                                      napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (JS_IsException(js_value)) {
    return NapiQuickjsStorePendingException(env);
  }

  if (napi_value existing = NapiQuickjsFindNapiValue(env, js_value)) {
    JS_FreeValue(env->context, js_value);
    *result = existing;
    return NapiQuickjsClearLastError(env);
  }

  napi_value value = NapiQuickjsMakeValue(env, kind);
  if (value == nullptr) {
    JS_FreeValue(env->context, js_value);
    return napi_generic_failure;
  }
  value->js_value = js_value;
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

void* NapiQuickjsJsIdentity(JSValueConst js_value) {
  if (!JS_IsObject(js_value) && !JS_IsSymbol(js_value)) return nullptr;
  return JS_VALUE_GET_PTR(js_value);
}

void NapiQuickjsRememberJsValue(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr || !value->has_js_value) return;
  void* identity = NapiQuickjsJsIdentity(value->js_value);
  if (identity != nullptr) env->js_to_napi_values[identity] = value;
}

napi_value NapiQuickjsFindNapiValue(napi_env env, JSValueConst js_value) {
  if (env == nullptr) return nullptr;
  void* identity = NapiQuickjsJsIdentity(js_value);
  if (identity == nullptr) return nullptr;
  auto it = env->js_to_napi_values.find(identity);
  return it != env->js_to_napi_values.end() ? it->second : nullptr;
}

napi_status NapiQuickjsStoreJsValue(napi_env env, JSValueConst js_value, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsWrapOwnedValue(
      env, KindForJsValue(env, js_value), JS_DupValue(env->context, js_value), result);
}

napi_status NapiQuickjsStoreOwnedJsValue(napi_env env, JSValue js_value, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsWrapOwnedValue(env, KindForJsValue(env, js_value), js_value, result);
}

JSValue NapiQuickjsValueToJsValue(napi_env env, napi_value value) {
  if (env == nullptr || env->context == nullptr || value == nullptr || !value->has_js_value) {
    return JS_UNDEFINED;
  }
  return JS_DupValue(env->context, value->js_value);
}

napi_status NapiQuickjsStorePendingException(napi_env env) {
  if (env == nullptr || env->context == nullptr) return napi_invalid_arg;
  JSValue exception = JS_GetException(env->context);
  napi_value pending = nullptr;
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, exception, &pending);
  if (status != napi_ok) {
    return status;
  }
  env->pending_exception = pending;
  return NapiQuickjsSetLastError(env, napi_pending_exception, "JavaScript exception is pending");
}

JSValue NapiQuickjsTakePendingException(napi_env env) {
  if (env == nullptr || env->context == nullptr || env->pending_exception == nullptr) {
    return JS_EXCEPTION;
  }
  JSValue exception = NapiQuickjsValueToJsValue(env, env->pending_exception);
  env->pending_exception = nullptr;
  return JS_Throw(env->context, exception);
}

JSValue NapiQuickjsCreateNativeFunction(napi_env env, napi_value function_value) {
  if (env == nullptr || function_value == nullptr || function_value->callback == nullptr) {
    return JS_EXCEPTION;
  }
  const size_t callback_id = env->native_callbacks.size();
  if (callback_id > std::numeric_limits<uint32_t>::max()) {
    return JS_ThrowInternalError(env->context, "Too many QuickJS native callbacks");
  }
  env->native_callbacks.push_back({function_value->callback, function_value->callback_data, function_value});
  JSValue data[] = {JS_NewUint32(env->context, static_cast<uint32_t>(callback_id))};
  JSValue function = JS_NewCFunctionData2(
      env->context,
      NapiQuickjsNativeFunctionBridge,
      function_value->debug_name.empty() ? "native" : function_value->debug_name.c_str(),
      0,
      0,
      1,
      data);
  if (JS_IsException(function)) env->native_callbacks.pop_back();
  return function;
}

napi_status NapiQuickjsDrainPromiseJobs(napi_env env) {
  if (env == nullptr || env->runtime == nullptr) return NapiQuickjsInvalidArg(env);

  JSContext* job_context = nullptr;
  for (;;) {
    int status = JS_ExecutePendingJob(env->runtime, &job_context);
    if (status <= 0) {
      if (status < 0) {
        napi_env job_env = env;
        if (job_context != nullptr) {
          if (auto* opaque_env = static_cast<napi_env>(JS_GetContextOpaque(job_context))) {
            job_env = opaque_env;
          }
        }
        return NapiQuickjsStorePendingException(job_env);
      }
      return NapiQuickjsClearLastError(env);
    }
  }
}

napi_status NapiQuickjsValueToPropertyKey(napi_env env,
                                          napi_value value,
                                          std::string* key) {
  if (env == nullptr || value == nullptr || key == nullptr) {
    return NapiQuickjsInvalidArg(env);
  }
  JSValue js_key = NapiQuickjsValueToJsValue(env, value);
  const char* str = JS_ToCString(env->context, js_key);
  if (str == nullptr) {
    JS_FreeValue(env->context, js_key);
    return NapiQuickjsSetLastError(env, napi_name_expected, "Property key must be convertible to string");
  }
  *key = str;
  JS_FreeCString(env->context, str);
  JS_FreeValue(env->context, js_key);
  return NapiQuickjsClearLastError(env);
}

napi_valuetype NapiQuickjsTypeOf(napi_value value) {
  if (value == nullptr) return napi_undefined;
  if (value->kind == NapiQuickjsValueKind::kExternal) return napi_external;
  if (!value->has_js_value) return napi_undefined;
  switch (value->kind) {
    case NapiQuickjsValueKind::kUndefined:
      return napi_undefined;
    case NapiQuickjsValueKind::kNull:
      return napi_null;
    case NapiQuickjsValueKind::kBoolean:
      return napi_boolean;
    case NapiQuickjsValueKind::kNumber:
    case NapiQuickjsValueKind::kDate:
      return napi_number;
    case NapiQuickjsValueKind::kString:
      return napi_string;
    case NapiQuickjsValueKind::kSymbol:
      return napi_symbol;
    case NapiQuickjsValueKind::kFunction:
      return napi_function;
    case NapiQuickjsValueKind::kBigInt:
      return napi_bigint;
    default:
      return napi_object;
  }
}

napi_status NapiQuickjsUnsupported(napi_env env, const char* message) {
  return NapiQuickjsSetLastError(
      env, napi_generic_failure, message != nullptr ? message : kGenericFailure);
}

napi_status NapiQuickjsInvalidArg(napi_env env, const char* message) {
  return NapiQuickjsSetLastError(
      env, napi_invalid_arg, message != nullptr ? message : "Invalid argument");
}

napi_status NapiQuickjsReleaseEnv(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;

  for (size_t i = env->cleanup_hooks.size(); i > 0; --i) {
    napi_cleanup_hook hook = env->cleanup_hooks[i - 1];
    if (hook != nullptr) hook(env->cleanup_hook_args[i - 1]);
  }

  if (env->cleanup_callback != nullptr) {
    env->cleanup_callback(env, env->cleanup_data);
  }

  for (napi_async_cleanup_hook_handle handle : env->async_cleanup_hooks) {
    if (handle != nullptr && !handle->removed && handle->hook != nullptr) {
      handle->hook(handle, handle->arg);
    }
    delete handle;
  }
  env->async_cleanup_hooks.clear();

  if (env->instance_data_finalize_cb != nullptr && env->instance_data != nullptr) {
    env->instance_data_finalize_cb(env, env->instance_data, env->instance_data_finalize_hint);
  }

  for (napi_value value : env->values) {
    if (value != nullptr && value->finalize_cb != nullptr && value->external_data != nullptr) {
      value->finalize_cb(env, value->external_data, value->finalize_hint);
    }
  }

  if (env->destroy_callback != nullptr) {
    env->destroy_callback(env, env->destroy_data);
  }

  if (env->context != nullptr &&
      env->host_defined_option_symbol != nullptr &&
      env->host_defined_option_symbol->has_js_value) {
    JSAtom host_defined_option_atom =
        JS_ValueToAtom(env->context, env->host_defined_option_symbol->js_value);
    if (host_defined_option_atom != JS_ATOM_NULL) {
      for (napi_value referrer : env->host_defined_option_referrers) {
        if (referrer != nullptr && referrer->has_js_value && JS_IsObject(referrer->js_value)) {
          (void)JS_DeleteProperty(env->context, referrer->js_value, host_defined_option_atom, 0);
        }
      }
      JS_FreeAtom(env->context, host_defined_option_atom);
    }
  }
  env->host_defined_option_referrers.clear();
  env->host_defined_option_symbol = nullptr;
  env->promise_context_frames.clear();
  env->promise_context_frame_stack.clear();

  if (env->context != nullptr) {
    env->pending_exception = nullptr;
    env->continuation_preserved_embedder_data = nullptr;
    env->global_object = nullptr;
    env->object_constructor = nullptr;
    env->object_prototype = nullptr;
  }

  const bool dump_leaks = std::getenv("EDGE_QUICKJS_DUMP_LEAKS") != nullptr;
  if (dump_leaks && env->runtime != nullptr) {
    JS_SetDumpFlags(env->runtime, JS_DUMP_LEAKS | JS_DUMP_MEM | JS_DUMP_OBJECTS);
  }

  for (napi_ref ref : env->refs) {
    if (ref != nullptr && ref->has_js_value) {
      JS_FreeValue(env->context, ref->js_value);
      ref->has_js_value = false;
    }
    delete ref;
  }
  env->refs.clear();

  for (napi_value value : env->values) {
    if (value != nullptr && value->has_js_value) {
      JS_FreeValue(env->context, value->js_value);
      value->has_js_value = false;
    }
    delete value;
  }
  env->values.clear();
  env->js_to_napi_values.clear();
  env->native_callbacks.clear();

  if (env->runtime != nullptr) {
    JS_RunGC(env->runtime);
  }

  if (env->context != nullptr) {
    JS_FreeContext(env->context);
    env->context = nullptr;
  }

  if (env->runtime != nullptr) {
    JS_RunGC(env->runtime);
  }

  if (env->runtime != nullptr) {
    JS_FreeRuntime(env->runtime);
    env->runtime = nullptr;
  }

  delete env;
  return napi_ok;
}

