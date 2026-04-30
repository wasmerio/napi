#include "napi_quickjs_env.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace {

size_t ResolveLength(const char* str, size_t length) {
  if (str == nullptr) return 0;
  return length == NAPI_AUTO_LENGTH ? std::strlen(str) : length;
}

napi_status StoreString(napi_env env, const char* str, size_t length, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  const char* source = str != nullptr ? str : "";
  JSValue value = JS_NewStringLen(env->context, source, ResolveLength(source, length));
  return NapiQuickjsStoreOwnedJsValue(env, value, result);
}

napi_status StoreError(napi_env env,
                       NapiQuickjsValueKind kind,
                       napi_value code,
                       napi_value msg,
                       napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue error = JS_NewError(env->context);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, error, result);
  if (status != napi_ok) return status;
  (*result)->kind = kind;
  if (msg != nullptr && napi_set_named_property(env, *result, "message", msg) != napi_ok) {
    return env->last_error.error_code;
  }
  if (code != nullptr && napi_set_named_property(env, *result, "code", code) != napi_ok) {
    return env->last_error.error_code;
  }
  return NapiQuickjsClearLastError(env);
}

size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int16_array:
    case napi_uint16_array:
    case napi_float16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

napi_typedarray_type NapiTypeFromQuickjsType(int type) {
  switch (type) {
    case JS_TYPED_ARRAY_INT8:
      return napi_int8_array;
    case JS_TYPED_ARRAY_UINT8:
      return napi_uint8_array;
    case JS_TYPED_ARRAY_UINT8C:
      return napi_uint8_clamped_array;
    case JS_TYPED_ARRAY_INT16:
      return napi_int16_array;
    case JS_TYPED_ARRAY_UINT16:
      return napi_uint16_array;
    case JS_TYPED_ARRAY_INT32:
      return napi_int32_array;
    case JS_TYPED_ARRAY_UINT32:
      return napi_uint32_array;
    case JS_TYPED_ARRAY_FLOAT32:
      return napi_float32_array;
    case JS_TYPED_ARRAY_FLOAT64:
      return napi_float64_array;
    case JS_TYPED_ARRAY_BIG_INT64:
      return napi_bigint64_array;
    case JS_TYPED_ARRAY_BIG_UINT64:
      return napi_biguint64_array;
    default:
      return napi_uint8_array;
  }
}

JSTypedArrayEnum QuickjsTypeFromNapiType(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
      return JS_TYPED_ARRAY_INT8;
    case napi_uint8_array:
      return JS_TYPED_ARRAY_UINT8;
    case napi_uint8_clamped_array:
      return JS_TYPED_ARRAY_UINT8C;
    case napi_int16_array:
      return JS_TYPED_ARRAY_INT16;
    case napi_uint16_array:
      return JS_TYPED_ARRAY_UINT16;
    case napi_int32_array:
      return JS_TYPED_ARRAY_INT32;
    case napi_uint32_array:
      return JS_TYPED_ARRAY_UINT32;
    case napi_float16_array:
      return JS_TYPED_ARRAY_FLOAT16;
    case napi_float32_array:
      return JS_TYPED_ARRAY_FLOAT32;
    case napi_float64_array:
      return JS_TYPED_ARRAY_FLOAT64;
    case napi_bigint64_array:
      return JS_TYPED_ARRAY_BIG_INT64;
    case napi_biguint64_array:
      return JS_TYPED_ARRAY_BIG_UINT64;
    default:
      return JS_TYPED_ARRAY_UINT8;
  }
}

bool IsObjectLike(napi_value value) {
  return value != nullptr && value->has_js_value && JS_IsObject(value->js_value);
}

bool IsHostDefinedOptionSymbol(napi_env env, napi_value key) {
  return env != nullptr && key != nullptr && key->has_js_value && JS_IsSymbol(key->js_value);
}

void RememberHostDefinedOptionReferrer(napi_env env, napi_value object, napi_value key) {
  if (env == nullptr || object == nullptr || key == nullptr) return;
  env->host_defined_option_symbol = key;
  if (std::find(env->host_defined_option_referrers.begin(),
                env->host_defined_option_referrers.end(),
                object) == env->host_defined_option_referrers.end()) {
    env->host_defined_option_referrers.push_back(object);
  }
}

int QuickjsPropertyFlags(napi_property_attributes attributes, bool has_value, bool has_getter, bool has_setter) {
  int flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
  flags |= (attributes & napi_configurable) ? JS_PROP_CONFIGURABLE : 0;
  flags |= (attributes & napi_enumerable) ? JS_PROP_ENUMERABLE : 0;
  if (has_value) {
    flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
    if (attributes & napi_writable) flags |= JS_PROP_WRITABLE;
  }
  if (has_getter) flags |= JS_PROP_HAS_GET;
  if (has_setter) flags |= JS_PROP_HAS_SET;
  return flags;
}

napi_status CreateNumber(napi_env env, double number, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsStoreOwnedJsValue(env, JS_NewNumber(env->context, number), result);
}

napi_status CreateArrayBufferValue(napi_env env, size_t length, void** data, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kArrayBuffer);
  if (value == nullptr) return napi_generic_failure;
  value->bytes.resize(length);
  value->byte_length = length;
  if (data != nullptr) *data = value->bytes.empty() ? nullptr : value->bytes.data();
  value->js_value = JS_NewArrayBuffer(env->context,
                                      value->bytes.empty() ? nullptr : value->bytes.data(),
                                      length,
                                      nullptr,
                                      nullptr,
                                      false);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status CreateBufferValue(napi_env env, size_t length, void** data, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kBuffer);
  if (value == nullptr) return napi_generic_failure;
  value->bytes.resize(length);
  value->byte_length = length;
  if (data != nullptr) *data = value->bytes.empty() ? nullptr : value->bytes.data();
  value->js_value = JS_NewUint8Array(env->context,
                                     value->bytes.empty() ? nullptr : value->bytes.data(),
                                     length,
                                     nullptr,
                                     nullptr,
                                     false);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->typedarray_type = napi_uint8_array;
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_value ObjectSetPrototypeOfCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2 ||
      !IsObjectLike(argv[0])) {
    return nullptr;
  }
  JSValue proto = NapiQuickjsValueToJsValue(env, argv[1]);
  if (JS_SetPrototype(env->context, argv[0]->js_value, proto) < 0) {
    JS_FreeValue(env->context, proto);
    (void)NapiQuickjsStorePendingException(env);
    return nullptr;
  }
  JS_FreeValue(env->context, proto);
  return argv[0];
}

napi_status EnsureGlobalObject(napi_env env) {
  if (env == nullptr) return napi_invalid_arg;
  if (env->global_object != nullptr) return NapiQuickjsClearLastError(env);

  JSValue global = JS_GetGlobalObject(env->context);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, global, &env->global_object);
  if (status != napi_ok) return status;
  JSValue object_ctor = JS_GetPropertyStr(env->context, env->global_object->js_value, "Object");
  if (JS_IsException(object_ctor)) return NapiQuickjsStorePendingException(env);
  status = NapiQuickjsStoreOwnedJsValue(env, object_ctor, &env->object_constructor);
  if (status != napi_ok) return status;
  JSValue object_proto = JS_GetPropertyStr(env->context, env->object_constructor->js_value, "prototype");
  if (JS_IsException(object_proto)) return NapiQuickjsStorePendingException(env);
  status = NapiQuickjsStoreOwnedJsValue(env, object_proto, &env->object_prototype);
  if (status != napi_ok) return status;

  napi_value set_prototype_of = nullptr;
  status = napi_create_function(env,
                                "setPrototypeOf",
                                NAPI_AUTO_LENGTH,
                                ObjectSetPrototypeOfCallback,
                                nullptr,
                                &set_prototype_of);
  if (status != napi_ok) return status;
  return napi_set_named_property(env, env->object_constructor, "setPrototypeOf", set_prototype_of);
}

}  // namespace

EXTERN_C_START

napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env env, const napi_extended_error_info** result) {
  if (result == nullptr) return napi_invalid_arg;
  if (env == nullptr) {
    static const napi_extended_error_info empty_error = {nullptr, nullptr, 0, napi_ok};
    *result = &empty_error;
    return napi_ok;
  }
  *result = &env->last_error;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsStoreOwnedJsValue(env, JS_UNDEFINED, result);
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsStoreOwnedJsValue(env, JS_NULL, result);
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_status status = EnsureGlobalObject(env);
  if (status != napi_ok) return status;
  *result = env->global_object;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool b, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsStoreOwnedJsValue(env, JS_NewBool(env->context, b), result);
}

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  return NapiQuickjsStoreOwnedJsValue(env, JS_NewObject(env->context), result);
}

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result) {
  return napi_create_array_with_length(env, 0, result);
}

napi_status NAPI_CDECL napi_create_array_with_length(napi_env env, size_t length, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue array = JS_NewArray(env->context);
  if (JS_IsException(array)) return NapiQuickjsStorePendingException(env);
  JSValue length_value = JS_NewUint32(env->context, static_cast<uint32_t>(length));
  if (JS_SetPropertyStr(env->context, array, "length", length_value) < 0) {
    JS_FreeValue(env->context, array);
    return NapiQuickjsStorePendingException(env);
  }
  return NapiQuickjsStoreOwnedJsValue(env, array, result);
}

napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value* result) {
  return CreateNumber(env, value, result);
}

napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value* result) {
  return CreateNumber(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value* result) {
  return CreateNumber(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value* result) {
  return CreateNumber(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_string_latin1(
    napi_env env, const char* str, size_t length, napi_value* result) {
  return StoreString(env, str, length, result);
}

napi_status NAPI_CDECL napi_create_string_utf8(
    napi_env env, const char* str, size_t length, napi_value* result) {
  return StoreString(env, str, length, result);
}

napi_status NAPI_CDECL napi_create_string_utf16(
    napi_env env, const char16_t* str, size_t length, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  size_t resolved = 0;
  if (str != nullptr) {
    if (length == NAPI_AUTO_LENGTH) {
      while (str[resolved] != 0) ++resolved;
    } else {
      resolved = length;
    }
  }
  std::string utf8;
  utf8.reserve(resolved);
  for (size_t i = 0; i < resolved; ++i) utf8.push_back(static_cast<char>(str[i] & 0xff));
  return StoreString(env, utf8.c_str(), utf8.size(), result);
}

napi_status NAPI_CDECL napi_create_symbol(napi_env env, napi_value description, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue symbol_ctor = JS_Eval(env->context, "Symbol", 6, "<napi_symbol>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(symbol_ctor)) return NapiQuickjsStorePendingException(env);
  JSValue arg = description != nullptr ? NapiQuickjsValueToJsValue(env, description) : JS_UNDEFINED;
  JSValue symbol = JS_Call(env->context, symbol_ctor, JS_UNDEFINED, description != nullptr ? 1 : 0, &arg);
  JS_FreeValue(env->context, symbol_ctor);
  if (description != nullptr) JS_FreeValue(env->context, arg);
  if (JS_IsException(symbol)) return NapiQuickjsStorePendingException(env);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, symbol, result);
  if (status == napi_ok) (*result)->kind = NapiQuickjsValueKind::kSymbol;
  return status;
}

napi_status NAPI_CDECL napi_create_function(
    napi_env env, const char* utf8name, size_t length, napi_callback cb, void* data, napi_value* result) {
  if (env == nullptr || result == nullptr || cb == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kFunction);
  if (value == nullptr) return napi_generic_failure;
  value->callback = cb;
  value->callback_data = data;
  if (utf8name != nullptr) value->debug_name.assign(utf8name, ResolveLength(utf8name, length));
  value->js_value = NapiQuickjsCreateNativeFunction(env, value);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  (void)JS_SetConstructorBit(env->context, value->js_value, true);
  JSValue prototype = JS_NewObject(env->context);
  if (JS_IsException(prototype)) return NapiQuickjsStorePendingException(env);
  if (JS_DefinePropertyValueStr(env->context,
                                value->js_value,
                                "prototype",
                                prototype,
                                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return StoreError(env, NapiQuickjsValueKind::kError, code, msg, result);
}

napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return StoreError(env, NapiQuickjsValueKind::kError, code, msg, result);
}

napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return StoreError(env, NapiQuickjsValueKind::kError, code, msg, result);
}

napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value, napi_valuetype* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = NapiQuickjsTypeOf(value);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_double(napi_env env, napi_value value, double* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (JS_ToFloat64(env->context, result, value->js_value) < 0) {
    return NapiQuickjsSetLastError(env, napi_number_expected, "Number expected");
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_int32(napi_env env, napi_value value, int32_t* result) {
  double number = 0;
  napi_status status = napi_get_value_double(env, value, &number);
  if (status == napi_ok) *result = static_cast<int32_t>(number);
  return status;
}

napi_status NAPI_CDECL napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result) {
  double number = 0;
  napi_status status = napi_get_value_double(env, value, &number);
  if (status == napi_ok) *result = static_cast<uint32_t>(number);
  return status;
}

napi_status NAPI_CDECL napi_get_value_int64(napi_env env, napi_value value, int64_t* result) {
  double number = 0;
  napi_status status = napi_get_value_double(env, value, &number);
  if (status == napi_ok) *result = static_cast<int64_t>(number);
  return status;
}

napi_status NAPI_CDECL napi_get_value_bool(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (value->kind != NapiQuickjsValueKind::kBoolean || !JS_IsBool(value->js_value)) {
    return NapiQuickjsSetLastError(env, napi_boolean_expected, "Boolean expected");
  }
  *result = JS_ToBool(env->context, value->js_value) != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_string_latin1(
    napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  return napi_get_value_string_utf8(env, value, buf, bufsize, result);
}

napi_status NAPI_CDECL napi_get_value_string_utf8(
    napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  if (env == nullptr || value == nullptr) return NapiQuickjsInvalidArg(env);
  if (value->kind != NapiQuickjsValueKind::kString && value->kind != NapiQuickjsValueKind::kSymbol) {
    return NapiQuickjsSetLastError(env, napi_string_expected, "String expected");
  }
  size_t length = 0;
  const char* str = JS_ToCStringLen(env->context, &length, value->js_value);
  if (str == nullptr) return NapiQuickjsSetLastError(env, napi_string_expected, "String expected");
  if (result != nullptr) *result = length;
  if (buf != nullptr && bufsize > 0) {
    const size_t copy = std::min(bufsize - 1, length);
    std::memcpy(buf, str, copy);
    buf[copy] = '\0';
  }
  JS_FreeCString(env->context, str);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf16(
    napi_env env, napi_value value, char16_t* buf, size_t bufsize, size_t* result) {
  if (env == nullptr || value == nullptr) return NapiQuickjsInvalidArg(env);
  size_t length = 0;
  const char* str = JS_ToCStringLen(env->context, &length, value->js_value);
  if (str == nullptr) return NapiQuickjsSetLastError(env, napi_string_expected, "String expected");
  if (result != nullptr) *result = length;
  if (buf != nullptr && bufsize > 0) {
    const size_t copy = std::min(bufsize - 1, length);
    for (size_t i = 0; i < copy; ++i) buf[i] = static_cast<unsigned char>(str[i]);
    buf[copy] = 0;
  }
  JS_FreeCString(env->context, str);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result) {
  if (value == nullptr) return NapiQuickjsInvalidArg(env);
  return napi_get_boolean(env, JS_ToBool(env->context, value->js_value) != 0, result);
}

napi_status NAPI_CDECL napi_coerce_to_number(napi_env env, napi_value value, napi_value* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue number = JS_ToNumber(env->context, value->js_value);
  if (JS_IsException(number)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, number, result);
}

napi_status NAPI_CDECL napi_coerce_to_object(napi_env env, napi_value value, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (IsObjectLike(value)) {
    *result = value;
    return NapiQuickjsClearLastError(env);
  }
  JSValue object = JS_ToObject(env->context, value != nullptr ? value->js_value : JS_UNDEFINED);
  if (JS_IsException(object)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, object, result);
}

napi_status NAPI_CDECL napi_coerce_to_string(napi_env env, napi_value value, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue string = JS_ToString(env->context, value != nullptr ? value->js_value : JS_UNDEFINED);
  if (JS_IsException(string)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, string, result);
}

napi_status NAPI_CDECL napi_get_prototype(napi_env env, napi_value object, napi_value* result) {
  if (env == nullptr || result == nullptr || !IsObjectLike(object)) return NapiQuickjsInvalidArg(env);
  JSValue proto = JS_GetPrototype(env->context, object->js_value);
  if (JS_IsException(proto)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, proto, result);
}

napi_status NAPI_CDECL node_api_set_prototype(napi_env env, napi_value object, napi_value value) {
  if (env == nullptr || !IsObjectLike(object)) return NapiQuickjsInvalidArg(env);
  JSValue proto = NapiQuickjsValueToJsValue(env, value);
  if (JS_SetPrototype(env->context, object->js_value, proto) < 0) {
    JS_FreeValue(env->context, proto);
    return NapiQuickjsStorePendingException(env);
  }
  JS_FreeValue(env->context, proto);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_property_names(napi_env env, napi_value object, napi_value* result) {
  if (env == nullptr || object == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSPropertyEnum* props = nullptr;
  uint32_t prop_count = 0;
  if (JS_GetOwnPropertyNames(env->context, &props, &prop_count, object->js_value,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  napi_status status = napi_create_array_with_length(env, prop_count, result);
  if (status == napi_ok) {
    for (uint32_t i = 0; i < prop_count; ++i) {
      JSValue key = JS_AtomToString(env->context, props[i].atom);
      if (!JS_IsException(key)) {
        napi_value key_value = nullptr;
        if (NapiQuickjsStoreOwnedJsValue(env, key, &key_value) == napi_ok) {
          (void)napi_set_element(env, *result, i, key_value);
        }
      }
    }
  }
  JS_FreePropertyEnum(env->context, props, prop_count);
  return status;
}

napi_status NAPI_CDECL napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
  if (env == nullptr || !IsObjectLike(object)) return NapiQuickjsInvalidArg(env);
  JSValue js_key = NapiQuickjsValueToJsValue(env, key);
  JSAtom atom = JS_ValueToAtom(env->context, js_key);
  JS_FreeValue(env->context, js_key);
  if (atom == JS_ATOM_NULL) return NapiQuickjsSetLastError(env, napi_name_expected, "Invalid property key");
  JSValue js_value = NapiQuickjsValueToJsValue(env, value);
  int rc = JS_SetProperty(env->context, object->js_value, atom, js_value);
  JS_FreeAtom(env->context, atom);
  if (rc < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  if (IsHostDefinedOptionSymbol(env, key)) {
    RememberHostDefinedOptionReferrer(env, object, key);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_has_property(napi_env env, napi_value object, napi_value key, bool* result) {
  if (env == nullptr || !IsObjectLike(object) || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue js_key = NapiQuickjsValueToJsValue(env, key);
  JSAtom atom = JS_ValueToAtom(env->context, js_key);
  JS_FreeValue(env->context, js_key);
  if (atom == JS_ATOM_NULL) return NapiQuickjsSetLastError(env, napi_name_expected, "Invalid property key");
  int has = JS_HasProperty(env->context, object->js_value, atom);
  JS_FreeAtom(env->context, atom);
  if (has < 0) return NapiQuickjsStorePendingException(env);
  *result = has != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_property(napi_env env, napi_value object, napi_value key, napi_value* result) {
  if (env == nullptr || !IsObjectLike(object) || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue js_key = NapiQuickjsValueToJsValue(env, key);
  JSAtom atom = JS_ValueToAtom(env->context, js_key);
  JS_FreeValue(env->context, js_key);
  if (atom == JS_ATOM_NULL) return NapiQuickjsSetLastError(env, napi_name_expected, "Invalid property key");
  JSValue prop = JS_GetProperty(env->context, object->js_value, atom);
  JS_FreeAtom(env->context, atom);
  if (JS_IsException(prop)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, prop, result);
}

napi_status NAPI_CDECL napi_delete_property(napi_env env, napi_value object, napi_value key, bool* result) {
  if (env == nullptr || !IsObjectLike(object)) return NapiQuickjsInvalidArg(env);
  JSValue js_key = NapiQuickjsValueToJsValue(env, key);
  JSAtom atom = JS_ValueToAtom(env->context, js_key);
  JS_FreeValue(env->context, js_key);
  if (atom == JS_ATOM_NULL) return NapiQuickjsSetLastError(env, napi_name_expected, "Invalid property key");
  int rc = JS_DeleteProperty(env->context, object->js_value, atom, 0);
  JS_FreeAtom(env->context, atom);
  if (rc < 0) return NapiQuickjsStorePendingException(env);
  if (result != nullptr) *result = rc != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_has_own_property(napi_env env, napi_value object, napi_value key, bool* result) {
  return napi_has_property(env, object, key, result);
}

napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object, const char* name, napi_value value) {
  if (env == nullptr || !IsObjectLike(object) || name == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue js_value = NapiQuickjsValueToJsValue(env, value);
  if (JS_SetPropertyStr(env->context, object->js_value, name, js_value) < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_has_named_property(napi_env env, napi_value object, const char* name, bool* result) {
  if (env == nullptr || !IsObjectLike(object) || name == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSAtom atom = JS_NewAtom(env->context, name);
  int has = JS_HasProperty(env->context, object->js_value, atom);
  JS_FreeAtom(env->context, atom);
  if (has < 0) return NapiQuickjsStorePendingException(env);
  *result = has != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object, const char* name, napi_value* result) {
  if (env == nullptr || !IsObjectLike(object) || name == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue value = JS_GetPropertyStr(env->context, object->js_value, name);
  if (JS_IsException(value)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, value, result);
}

napi_status NAPI_CDECL napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value) {
  if (env == nullptr || object == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue js_value = NapiQuickjsValueToJsValue(env, value);
  if (JS_SetPropertyUint32(env->context, object->js_value, index, js_value) < 0) {
    return NapiQuickjsStorePendingException(env);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_has_element(napi_env env, napi_value object, uint32_t index, bool* result) {
  if (env == nullptr || object == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSAtom atom = JS_NewAtomUInt32(env->context, index);
  int has = JS_HasProperty(env->context, object->js_value, atom);
  JS_FreeAtom(env->context, atom);
  if (has < 0) return NapiQuickjsStorePendingException(env);
  *result = has != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value* result) {
  if (env == nullptr || object == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue value = JS_GetPropertyUint32(env->context, object->js_value, index);
  if (JS_IsException(value)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, value, result);
}

napi_status NAPI_CDECL napi_delete_element(napi_env env, napi_value object, uint32_t index, bool* result) {
  if (env == nullptr || object == nullptr) return NapiQuickjsInvalidArg(env);
  JSAtom atom = JS_NewAtomUInt32(env->context, index);
  int rc = JS_DeleteProperty(env->context, object->js_value, atom, 0);
  JS_FreeAtom(env->context, atom);
  if (rc < 0) return NapiQuickjsStorePendingException(env);
  if (result != nullptr) *result = rc != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_define_properties(
    napi_env env, napi_value object, size_t property_count, const napi_property_descriptor* properties) {
  if (env == nullptr || !IsObjectLike(object)) return NapiQuickjsInvalidArg(env);
  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor& descriptor = properties[i];
    if (descriptor.utf8name == nullptr && descriptor.name == nullptr) {
      return NapiQuickjsSetLastError(env, napi_name_expected, "Property descriptor is missing a name");
    }

    JSAtom atom = JS_ATOM_NULL;
    if (descriptor.utf8name != nullptr) {
      atom = JS_NewAtom(env->context, descriptor.utf8name);
    } else {
      JSValue js_name = NapiQuickjsValueToJsValue(env, descriptor.name);
      atom = JS_ValueToAtom(env->context, js_name);
      JS_FreeValue(env->context, js_name);
    }
    if (atom == JS_ATOM_NULL) {
      return NapiQuickjsSetLastError(env, napi_name_expected, "Invalid property key");
    }

    napi_value value = descriptor.value;
    if (descriptor.method != nullptr) {
      napi_status status = napi_create_function(
          env, descriptor.utf8name, NAPI_AUTO_LENGTH, descriptor.method, descriptor.data, &value);
      if (status != napi_ok) {
        JS_FreeAtom(env->context, atom);
        return status;
      }
    }

    napi_value getter = nullptr;
    if (descriptor.getter != nullptr) {
      napi_status status = napi_create_function(
          env, descriptor.utf8name, NAPI_AUTO_LENGTH, descriptor.getter, descriptor.data, &getter);
      if (status != napi_ok) {
        JS_FreeAtom(env->context, atom);
        return status;
      }
    }

    napi_value setter = nullptr;
    if (descriptor.setter != nullptr) {
      napi_status status = napi_create_function(
          env, descriptor.utf8name, NAPI_AUTO_LENGTH, descriptor.setter, descriptor.data, &setter);
      if (status != napi_ok) {
        JS_FreeAtom(env->context, atom);
        return status;
      }
    }

    const bool has_value = value != nullptr;
    const bool has_getter = getter != nullptr;
    const bool has_setter = setter != nullptr;
    if (!has_value && !has_getter && !has_setter) {
      JS_FreeAtom(env->context, atom);
      continue;
    }

    JSValue js_value = has_value ? NapiQuickjsValueToJsValue(env, value) : JS_UNDEFINED;
    JSValue js_getter = has_getter ? NapiQuickjsValueToJsValue(env, getter) : JS_UNDEFINED;
    JSValue js_setter = has_setter ? NapiQuickjsValueToJsValue(env, setter) : JS_UNDEFINED;
    int flags = QuickjsPropertyFlags(descriptor.attributes, has_value, has_getter, has_setter);
    int rc = JS_DefineProperty(env->context, object->js_value, atom, js_value, js_getter, js_setter, flags);
    if (has_value) JS_FreeValue(env->context, js_value);
    if (has_getter) JS_FreeValue(env->context, js_getter);
    if (has_setter) JS_FreeValue(env->context, js_setter);
    JS_FreeAtom(env->context, atom);
    if (rc < 0) {
      return NapiQuickjsStorePendingException(env);
    }
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->has_js_value && JS_IsArray(value->js_value);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_array_length(napi_env env, napi_value value, uint32_t* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue length_value = JS_GetPropertyStr(env->context, value->js_value, "length");
  if (JS_IsException(length_value)) return NapiQuickjsStorePendingException(env);
  uint32_t length = 0;
  if (JS_ToUint32(env->context, &length, length_value) < 0) {
    JS_FreeValue(env->context, length_value);
    return NapiQuickjsSetLastError(env, napi_array_expected, "Failed to get array length");
  }
  JS_FreeValue(env->context, length_value);
  *result = length;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (lhs == rhs) {
    *result = true;
  } else if (lhs == nullptr || rhs == nullptr) {
    *result = false;
  } else {
    *result = JS_IsStrictEqual(env->context, lhs->js_value, rhs->js_value);
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_call_function(
    napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result) {
  if (env != nullptr && func != nullptr && func->has_js_value &&
      JS_IsFunction(env->context, func->js_value)) {
    std::vector<JSValue> js_args;
    js_args.reserve(argc);
    for (size_t i = 0; i < argc; ++i) {
      js_args.push_back(NapiQuickjsValueToJsValue(env, argv != nullptr ? argv[i] : nullptr));
      if (JS_IsException(js_args.back())) {
        for (JSValue arg : js_args) JS_FreeValue(env->context, arg);
        return NapiQuickjsSetLastError(env, napi_generic_failure, "Failed to bridge QuickJS argument");
      }
    }
    JSValue this_value = recv != nullptr ? NapiQuickjsValueToJsValue(env, recv) : JS_UNDEFINED;
    JSValue value = JS_Call(env->context,
                            func->js_value,
                            this_value,
                            static_cast<int>(js_args.size()),
                            js_args.empty() ? nullptr : js_args.data());
    JS_FreeValue(env->context, this_value);
    for (JSValue arg : js_args) JS_FreeValue(env->context, arg);
    if (JS_IsException(value)) return NapiQuickjsStorePendingException(env);
    return result != nullptr ? NapiQuickjsStoreOwnedJsValue(env, value, result)
                             : (JS_FreeValue(env->context, value), NapiQuickjsClearLastError(env));
  }
  if (env == nullptr || func == nullptr || func->callback == nullptr) {
    return NapiQuickjsSetLastError(env, napi_function_expected, "Function expected");
  }
  auto* info = new (std::nothrow) napi_callback_info__();
  if (info == nullptr) return NapiQuickjsSetLastError(env, napi_generic_failure, "Failed to allocate callback info");
  info->env = env;
  info->this_arg = recv;
  info->data = func->callback_data;
  if (argv != nullptr) info->args.assign(argv, argv + argc);
  napi_value value = func->callback(env, info);
  delete info;
  if (result != nullptr) *result = value;
  return env->pending_exception == nullptr ? NapiQuickjsClearLastError(env) : napi_pending_exception;
}

napi_status NAPI_CDECL napi_new_instance(
    napi_env env, napi_value constructor, size_t argc, const napi_value* argv, napi_value* result) {
  if (env == nullptr || constructor == nullptr || result == nullptr ||
      !constructor->has_js_value ||
      !JS_IsConstructor(env->context, constructor->js_value)) {
    return NapiQuickjsSetLastError(env, napi_function_expected, "Constructor expected");
  }
  std::vector<JSValue> js_args;
  js_args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) {
    js_args.push_back(NapiQuickjsValueToJsValue(env, argv != nullptr ? argv[i] : nullptr));
    if (JS_IsException(js_args.back())) {
      for (JSValue arg : js_args) JS_FreeValue(env->context, arg);
      return NapiQuickjsSetLastError(env, napi_generic_failure, "Failed to bridge QuickJS constructor argument");
    }
  }
  JSValue instance = JS_CallConstructor(
      env->context,
      constructor->js_value,
      static_cast<int>(js_args.size()),
      js_args.empty() ? nullptr : js_args.data());
  for (JSValue arg : js_args) JS_FreeValue(env->context, arg);
  if (JS_IsException(instance)) return NapiQuickjsStorePendingException(env);
  return NapiQuickjsStoreOwnedJsValue(env, instance, result);
}

napi_status NAPI_CDECL napi_instanceof(napi_env env, napi_value object, napi_value constructor, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (object == nullptr || constructor == nullptr || !object->has_js_value || !constructor->has_js_value) {
    *result = false;
    return NapiQuickjsClearLastError(env);
  }
  int rc = JS_IsInstanceOf(env->context, object->js_value, constructor->js_value);
  if (rc < 0) return NapiQuickjsStorePendingException(env);
  *result = rc != 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_cb_info(
    napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv, napi_value* this_arg, void** data) {
  if (env == nullptr || cbinfo == nullptr) return NapiQuickjsInvalidArg(env);
  if (argc != nullptr) {
    const size_t copy = argv != nullptr ? std::min(*argc, cbinfo->args.size()) : 0;
    for (size_t i = 0; i < copy; ++i) argv[i] = cbinfo->args[i];
    *argc = cbinfo->args.size();
  }
  if (this_arg != nullptr) *this_arg = cbinfo->this_arg;
  if (data != nullptr) *data = cbinfo->data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_new_target(napi_env env, napi_callback_info cbinfo, napi_value* result) {
  if (env == nullptr || cbinfo == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = cbinfo->new_target;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_define_class(
    napi_env env,
    const char* utf8name,
    size_t length,
    napi_callback constructor,
    void* data,
    size_t property_count,
    const napi_property_descriptor* properties,
    napi_value* result) {
  napi_status status = napi_create_function(env, utf8name, length, constructor, data, result);
  if (status == napi_ok && result != nullptr && *result != nullptr) {
    (*result)->is_class_constructor = true;
  }
  if (status == napi_ok && properties != nullptr && property_count > 0) {
    napi_value prototype = nullptr;
    status = napi_get_named_property(env, *result, "prototype", &prototype);
    if (status == napi_ok) status = napi_define_properties(env, prototype, property_count, properties);
  }
  return status;
}

napi_status NAPI_CDECL napi_wrap(
    napi_env env, napi_value js_object, void* native_object, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_ref* result) {
  if (env == nullptr || !IsObjectLike(js_object)) return NapiQuickjsInvalidArg(env);
  js_object->external_data = native_object;
  js_object->finalize_cb = finalize_cb;
  js_object->finalize_hint = finalize_hint;
  return result != nullptr ? napi_create_reference(env, js_object, 0, result) : NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void** result) {
  if (env == nullptr || js_object == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = js_object->external_data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
  napi_status status = napi_unwrap(env, js_object, result);
  if (status == napi_ok) {
    js_object->external_data = nullptr;
    js_object->finalize_cb = nullptr;
  }
  return status;
}

napi_status NAPI_CDECL napi_create_external(
    napi_env env, void* data, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kExternal);
  if (value == nullptr) return napi_generic_failure;
  value->external_data = data;
  value->finalize_cb = finalize_cb;
  value->finalize_hint = finalize_hint;
  value->js_value = JS_NewObject(env->context);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_external(napi_env env, napi_value value, void** result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value->external_data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  auto* ref = new (std::nothrow) napi_ref__();
  if (ref == nullptr) return NapiQuickjsSetLastError(env, napi_generic_failure, "Failed to allocate napi_ref");
  ref->env = env;
  ref->value = value;
  ref->refcount = initial_refcount;
  ref->js_value = NapiQuickjsValueToJsValue(env, value);
  ref->has_js_value = true;
  env->refs.push_back(ref);
  *result = ref;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return NapiQuickjsInvalidArg(env);
  auto it = std::find(env->refs.begin(), env->refs.end(), ref);
  if (it != env->refs.end()) env->refs.erase(it);
  if (ref->has_js_value) JS_FreeValue(env->context, ref->js_value);
  delete ref;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result) {
  if (env == nullptr || ref == nullptr) return NapiQuickjsInvalidArg(env);
  if (ref->refcount < std::numeric_limits<uint32_t>::max()) ++ref->refcount;
  if (result != nullptr) *result = ref->refcount;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result) {
  if (env == nullptr || ref == nullptr) return NapiQuickjsInvalidArg(env);
  if (ref->refcount > 0) --ref->refcount;
  if (result != nullptr) *result = ref->refcount;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result) {
  if (env == nullptr || ref == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = ref->value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  auto* scope = new (std::nothrow) napi_handle_scope__();
  if (scope == nullptr) return napi_generic_failure;
  scope->env = env;
  *result = scope;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  if (env == nullptr || scope == nullptr) return NapiQuickjsInvalidArg(env);
  delete scope;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  auto* scope = new (std::nothrow) napi_escapable_handle_scope__();
  if (scope == nullptr) return napi_generic_failure;
  scope->env = env;
  *result = scope;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
  if (env == nullptr || scope == nullptr) return NapiQuickjsInvalidArg(env);
  delete scope;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_escape_handle(
    napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value* result) {
  if (env == nullptr || scope == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (scope->escaped) return NapiQuickjsSetLastError(env, napi_escape_called_twice, "Escape called twice");
  scope->escaped = true;
  *result = escapee;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  if (env == nullptr || error == nullptr) return NapiQuickjsInvalidArg(env);
  env->pending_exception = error;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_throw_error(napi_env env, const char* code, const char* msg) {
  napi_value code_value = nullptr;
  napi_value msg_value = nullptr;
  napi_value error = nullptr;
  if (code != nullptr) StoreString(env, code, NAPI_AUTO_LENGTH, &code_value);
  StoreString(env, msg != nullptr ? msg : "", NAPI_AUTO_LENGTH, &msg_value);
  napi_create_error(env, code_value, msg_value, &error);
  return napi_throw(env, error);
}

napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char* code, const char* msg) {
  return napi_throw_error(env, code, msg);
}

napi_status NAPI_CDECL napi_throw_range_error(napi_env env, const char* code, const char* msg) {
  return napi_throw_error(env, code, msg);
}

napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->has_js_value && JS_IsError(value->js_value);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = env->pending_exception != nullptr;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = env->pending_exception;
  env->pending_exception = nullptr;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->has_js_value && JS_IsArrayBuffer(value->js_value);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env, size_t byte_length, void** data, napi_value* result) {
  return CreateArrayBufferValue(env, byte_length, data, result);
}

napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env, void* external_data, size_t byte_length, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kArrayBuffer);
  if (value == nullptr) return napi_generic_failure;
  value->external_data = external_data;
  value->byte_length = byte_length;
  value->finalize_cb = finalize_cb;
  value->finalize_hint = finalize_hint;
  value->js_value = JS_NewArrayBuffer(env->context,
                                      static_cast<uint8_t*>(external_data),
                                      byte_length,
                                      nullptr,
                                      nullptr,
                                      false);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data, size_t* byte_length) {
  if (env == nullptr || arraybuffer == nullptr || !arraybuffer->has_js_value) return NapiQuickjsInvalidArg(env);
  if (!JS_IsArrayBuffer(arraybuffer->js_value)) {
    return NapiQuickjsSetLastError(env, napi_arraybuffer_expected, "ArrayBuffer expected");
  }
  size_t size = 0;
  uint8_t* raw = JS_GetArrayBuffer(env->context, &size, arraybuffer->js_value);
  if (data != nullptr) *data = raw;
  if (byte_length != nullptr) *byte_length = size;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL node_api_is_sharedarraybuffer(napi_env env, napi_value value, bool* result) {
  (void)value;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = false;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL node_api_create_sharedarraybuffer(
    napi_env env, size_t byte_length, void** data, napi_value* result) {
  return CreateArrayBufferValue(env, byte_length, data, result);
}

napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->has_js_value && JS_GetTypedArrayType(value->js_value) >= 0;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_typedarray(
    napi_env env, napi_typedarray_type type, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value* result) {
  if (env == nullptr || arraybuffer == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kTypedArray);
  if (value == nullptr) return napi_generic_failure;
  value->typedarray_type = type;
  value->arraybuffer = arraybuffer;
  value->byte_offset = byte_offset;
  value->byte_length = length;
  JSValue args[3] = {
      NapiQuickjsValueToJsValue(env, arraybuffer),
      JS_NewUint32(env->context, static_cast<uint32_t>(byte_offset)),
      JS_NewUint32(env->context, static_cast<uint32_t>(length)),
  };
  value->js_value = JS_NewTypedArray(env->context, 3, args, QuickjsTypeFromNapiType(type));
  for (JSValue arg : args) JS_FreeValue(env->context, arg);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_typedarray_info(
    napi_env env, napi_value typedarray, napi_typedarray_type* type, size_t* length, void** data, napi_value* arraybuffer, size_t* byte_offset) {
  if (env == nullptr || typedarray == nullptr || !typedarray->has_js_value) return NapiQuickjsInvalidArg(env);
  int quickjs_type = JS_GetTypedArrayType(typedarray->js_value);
  if (quickjs_type < 0) {
    return NapiQuickjsSetLastError(env, napi_invalid_arg, "TypedArray expected");
  }

  size_t view_byte_offset = 0;
  size_t view_byte_length = 0;
  size_t bytes_per_element = 1;
  JSValue buffer = JS_GetTypedArrayBuffer(
      env->context, typedarray->js_value, &view_byte_offset, &view_byte_length, &bytes_per_element);
  if (JS_IsException(buffer)) return NapiQuickjsStorePendingException(env);

  napi_value buffer_value = nullptr;
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, buffer, &buffer_value);
  if (status != napi_ok) return status;
  if (buffer_value != nullptr) buffer_value->kind = NapiQuickjsValueKind::kArrayBuffer;

  if (type != nullptr) *type = NapiTypeFromQuickjsType(quickjs_type);
  if (length != nullptr) *length = bytes_per_element == 0 ? 0 : view_byte_length / bytes_per_element;
  if (arraybuffer != nullptr) *arraybuffer = buffer_value;
  if (byte_offset != nullptr) *byte_offset = view_byte_offset;
  if (data != nullptr) {
    size_t buffer_size = 0;
    uint8_t* raw = JS_GetArrayBuffer(env->context, &buffer_size, buffer_value->js_value);
    *data = raw == nullptr ? nullptr : raw + view_byte_offset;
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_dataview(
    napi_env env, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value* result) {
  napi_status status = napi_create_typedarray(env, napi_uint8_array, length, arraybuffer, byte_offset, result);
  if (status == napi_ok) (*result)->kind = NapiQuickjsValueKind::kDataView;
  return status;
}

napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->kind == NapiQuickjsValueKind::kDataView;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_dataview_info(
    napi_env env, napi_value dataview, size_t* bytelength, void** data, napi_value* arraybuffer, size_t* byte_offset) {
  return napi_get_typedarray_info(env, dataview, nullptr, bytelength, data, arraybuffer, byte_offset);
}

napi_status NAPI_CDECL napi_get_version(node_api_basic_env env, uint32_t* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = NAPI_VERSION;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_promise(napi_env env, napi_deferred* deferred, napi_value* promise) {
  if (env == nullptr || deferred == nullptr || promise == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue js_promise = JS_NewPromiseCapability(env->context, funcs);
  if (JS_IsException(js_promise)) return NapiQuickjsStorePendingException(env);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, js_promise, promise);
  if (status != napi_ok) {
    JS_FreeValue(env->context, funcs[0]);
    JS_FreeValue(env->context, funcs[1]);
    return status;
  }
  (*promise)->kind = NapiQuickjsValueKind::kPromise;
  auto* d = new (std::nothrow) napi_deferred__();
  if (d == nullptr) {
    JS_FreeValue(env->context, funcs[0]);
    JS_FreeValue(env->context, funcs[1]);
    return napi_generic_failure;
  }
  d->env = env;
  d->promise = *promise;
  status = NapiQuickjsStoreOwnedJsValue(env, funcs[0], &d->resolve);
  if (status != napi_ok) {
    JS_FreeValue(env->context, funcs[1]);
    delete d;
    return status;
  }
  status = NapiQuickjsStoreOwnedJsValue(env, funcs[1], &d->reject);
  if (status != napi_ok) {
    delete d;
    return status;
  }
  *deferred = d;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_resolve_deferred(napi_env env, napi_deferred deferred, napi_value resolution) {
  if (env == nullptr || deferred == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  napi_value argv[1] = {resolution != nullptr ? resolution : undefined};
  napi_status status = napi_call_function(env, undefined, deferred->resolve, 1, argv, nullptr);
  delete deferred;
  return status == napi_ok ? NapiQuickjsClearLastError(env) : status;
}

napi_status NAPI_CDECL napi_reject_deferred(napi_env env, napi_deferred deferred, napi_value rejection) {
  if (env == nullptr || deferred == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  napi_value argv[1] = {rejection != nullptr ? rejection : undefined};
  napi_status status = napi_call_function(env, undefined, deferred->reject, 1, argv, nullptr);
  delete deferred;
  return status == napi_ok ? NapiQuickjsClearLastError(env) : status;
}

napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool* is_promise) {
  if (env == nullptr || is_promise == nullptr) return NapiQuickjsInvalidArg(env);
  *is_promise = value != nullptr && value->kind == NapiQuickjsValueKind::kPromise;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_run_script(napi_env env, napi_value script, napi_value* result) {
  if (env == nullptr || env->context == nullptr || script == nullptr) return NapiQuickjsInvalidArg(env);
  size_t source_len = 0;
  const char* source = JS_ToCStringLen(env->context, &source_len, script->js_value);
  if (source == nullptr) return NapiQuickjsSetLastError(env, napi_string_expected, "Script source must be a string");

  JSValue value = JS_Eval(
      env->context, source, source_len, "<napi_run_script>", JS_EVAL_TYPE_GLOBAL);
  JS_FreeCString(env->context, source);
  if (JS_IsException(value)) {
    return NapiQuickjsStorePendingException(env);
  }
  napi_status drain_status = NapiQuickjsDrainPromiseJobs(env);
  if (drain_status != napi_ok) {
    JS_FreeValue(env->context, value);
    return drain_status;
  }

  return result != nullptr ? NapiQuickjsStoreOwnedJsValue(env, value, result)
                           : (JS_FreeValue(env->context, value), NapiQuickjsClearLastError(env));
}

napi_status NAPI_CDECL napi_adjust_external_memory(node_api_basic_env env, int64_t change_in_bytes, int64_t* adjusted_value) {
  if (env == nullptr) return NapiQuickjsInvalidArg(env);
  if (adjusted_value != nullptr) *adjusted_value = change_in_bytes;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_date(napi_env env, double time, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue date_ctor = JS_Eval(env->context, "Date", 4, "<napi_date>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(date_ctor)) return NapiQuickjsStorePendingException(env);
  JSValue arg = JS_NewFloat64(env->context, time);
  JSValue date = JS_CallConstructor(env->context, date_ctor, 1, &arg);
  JS_FreeValue(env->context, arg);
  JS_FreeValue(env->context, date_ctor);
  if (JS_IsException(date)) return NapiQuickjsStorePendingException(env);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, date, result);
  if (status == napi_ok) (*result)->kind = NapiQuickjsValueKind::kDate;
  return status;
}

napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool* is_date) {
  if (env == nullptr || is_date == nullptr) return NapiQuickjsInvalidArg(env);
  *is_date = value != nullptr && value->kind == NapiQuickjsValueKind::kDate;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value, double* result) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue get_time = JS_GetPropertyStr(env->context, value->js_value, "getTime");
  if (JS_IsException(get_time)) return NapiQuickjsStorePendingException(env);
  JSValue time = JS_Call(env->context, get_time, value->js_value, 0, nullptr);
  JS_FreeValue(env->context, get_time);
  if (JS_IsException(time)) return NapiQuickjsStorePendingException(env);
  int rc = JS_ToFloat64(env->context, result, time);
  JS_FreeValue(env->context, time);
  return rc < 0 ? NapiQuickjsSetLastError(env, napi_number_expected, "Date value expected")
                : NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_add_finalizer(
    napi_env env, napi_value js_object, void* finalize_data, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_ref* result) {
  if (env == nullptr || js_object == nullptr) return NapiQuickjsInvalidArg(env);
  js_object->external_data = finalize_data;
  js_object->finalize_cb = finalize_cb;
  js_object->finalize_hint = finalize_hint;
  return result != nullptr ? napi_create_reference(env, js_object, 0, result) : NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env, int64_t value, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  JSValue bigint = JS_NewBigInt64(env->context, value);
  napi_status status = NapiQuickjsStoreOwnedJsValue(env, bigint, result);
  if (status == napi_ok) (*result)->kind = NapiQuickjsValueKind::kBigInt;
  return status;
}

napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value* result) {
  return napi_create_bigint_int64(env, static_cast<int64_t>(value), result);
}

napi_status NAPI_CDECL napi_create_bigint_words(
    napi_env env, int sign_bit, size_t word_count, const uint64_t* words, napi_value* result) {
  int64_t value = (word_count > 0 && words != nullptr) ? static_cast<int64_t>(words[0]) : 0;
  if (sign_bit != 0) value = -value;
  return napi_create_bigint_int64(env, value, result);
}

napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t* result, bool* lossless) {
  if (env == nullptr || value == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  if (JS_ToBigInt64(env->context, result, value->js_value) < 0) {
    return NapiQuickjsSetLastError(env, napi_bigint_expected, "BigInt expected");
  }
  if (lossless != nullptr) *lossless = true;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t* result, bool* lossless) {
  int64_t signed_value = 0;
  napi_status status = napi_get_value_bigint_int64(env, value, &signed_value, lossless);
  if (status == napi_ok) *result = static_cast<uint64_t>(signed_value);
  return status;
}

napi_status NAPI_CDECL napi_get_value_bigint_words(
    napi_env env, napi_value value, int* sign_bit, size_t* word_count, uint64_t* words) {
  if (env == nullptr || value == nullptr || word_count == nullptr) return NapiQuickjsInvalidArg(env);
  int64_t signed_value = 0;
  if (JS_ToBigInt64(env->context, &signed_value, value->js_value) < 0) {
    return NapiQuickjsSetLastError(env, napi_bigint_expected, "BigInt expected");
  }
  if (sign_bit != nullptr) *sign_bit = signed_value < 0 ? 1 : 0;
  if (words != nullptr && *word_count > 0) words[0] = static_cast<uint64_t>(std::llabs(signed_value));
  *word_count = 1;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_all_property_names(
    napi_env env, napi_value object, napi_key_collection_mode key_mode, napi_key_filter key_filter, napi_key_conversion key_conversion, napi_value* result) {
  (void)key_mode;
  (void)key_filter;
  (void)key_conversion;
  return napi_get_property_names(env, object, result);
}

napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env env, void* data, napi_finalize finalize_cb, void* finalize_hint) {
  if (env == nullptr) return NapiQuickjsInvalidArg(env);
  env->instance_data = data;
  env->instance_data_finalize_cb = finalize_cb;
  env->instance_data_finalize_hint = finalize_hint;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env env, void** data) {
  if (env == nullptr || data == nullptr) return NapiQuickjsInvalidArg(env);
  *data = env->instance_data;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  if (env == nullptr || arraybuffer == nullptr) return NapiQuickjsInvalidArg(env);
  arraybuffer->bytes.clear();
  arraybuffer->byte_length = 0;
  arraybuffer->external_data = nullptr;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->kind == NapiQuickjsValueKind::kArrayBuffer &&
            value->byte_length == 0 && value->external_data == nullptr;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_type_tag_object(napi_env env, napi_value value, const napi_type_tag* type_tag) {
  (void)value;
  (void)type_tag;
  return NapiQuickjsUnsupported(env, "QuickJS provider does not support object type tags yet");
}

napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env, napi_value value, const napi_type_tag* type_tag, bool* result) {
  (void)value;
  (void)type_tag;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = false;
  return NapiQuickjsUnsupported(env, "QuickJS provider does not support object type tags yet");
}

napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  if (env == nullptr || object == nullptr) return napi_invalid_arg;
  JSValue freeze = JS_Eval(env->context, "Object.freeze", 13, "<napi_freeze>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(freeze)) return NapiQuickjsStorePendingException(env);
  JSValue arg = NapiQuickjsValueToJsValue(env, object);
  JSValue out = JS_Call(env->context, freeze, JS_UNDEFINED, 1, &arg);
  JS_FreeValue(env->context, freeze);
  JS_FreeValue(env->context, arg);
  if (JS_IsException(out)) return NapiQuickjsStorePendingException(env);
  JS_FreeValue(env->context, out);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  return napi_object_freeze(env, object);
}

void NAPI_CDECL napi_module_register(napi_module* mod) {
  (void)mod;
}

void NAPI_CDECL napi_fatal_error(
    const char* location, size_t location_len, const char* message, size_t message_len) {
  (void)location;
  (void)location_len;
  (void)message;
  (void)message_len;
  std::abort();
}

napi_status NAPI_CDECL napi_async_init(
    napi_env env, napi_value async_resource, napi_value async_resource_name, napi_async_context* result) {
  (void)async_resource;
  (void)async_resource_name;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = reinterpret_cast<napi_async_context>(new (std::nothrow) int(0));
  return *result != nullptr ? NapiQuickjsClearLastError(env) : napi_generic_failure;
}

napi_status NAPI_CDECL napi_async_destroy(napi_env env, napi_async_context async_context) {
  if (env == nullptr || async_context == nullptr) return NapiQuickjsInvalidArg(env);
  delete reinterpret_cast<int*>(async_context);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_make_callback(
    napi_env env, napi_async_context async_context, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result) {
  (void)async_context;
  napi_status status = napi_call_function(env, recv, func, argc, argv, result);
  if (status != napi_ok) return status;
  return NapiQuickjsDrainPromiseJobs(env);
}

napi_status NAPI_CDECL napi_create_buffer(napi_env env, size_t length, void** data, napi_value* result) {
  return CreateBufferValue(env, length, data, result);
}

napi_status NAPI_CDECL napi_create_external_buffer(
    napi_env env, size_t length, void* data, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_value* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  napi_value value = NapiQuickjsMakeValue(env, NapiQuickjsValueKind::kBuffer);
  if (value == nullptr) return napi_generic_failure;
  value->external_data = data;
  value->byte_length = length;
  value->finalize_cb = finalize_cb;
  value->finalize_hint = finalize_hint;
  value->typedarray_type = napi_uint8_array;
  value->js_value = JS_NewUint8Array(env->context,
                                     static_cast<uint8_t*>(data),
                                     length,
                                     nullptr,
                                     nullptr,
                                     false);
  if (JS_IsException(value->js_value)) return NapiQuickjsStorePendingException(env);
  value->has_js_value = true;
  NapiQuickjsRememberJsValue(env, value);
  *result = value;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_buffer_copy(
    napi_env env, size_t length, const void* data, void** result_data, napi_value* result) {
  napi_status status = CreateBufferValue(env, length, result_data, result);
  if (status == napi_ok && data != nullptr && result_data != nullptr && *result_data != nullptr) {
    std::memcpy(*result_data, data, length);
  }
  return status;
}

napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = value != nullptr && value->has_js_value && JS_GetTypedArrayType(value->js_value) == JS_TYPED_ARRAY_UINT8;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_buffer_info(napi_env env, napi_value value, void** data, size_t* length) {
  if (env == nullptr || value == nullptr || !value->has_js_value) return NapiQuickjsInvalidArg(env);
  size_t size = 0;
  uint8_t* raw = JS_GetUint8Array(env->context, &size, value->js_value);
  if (raw == nullptr) {
    return NapiQuickjsSetLastError(env, napi_invalid_arg, "Buffer expected");
  }
  if (data != nullptr) *data = raw;
  if (length != nullptr) *length = size;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_async_work(
    napi_env env, napi_value async_resource, napi_value async_resource_name, napi_async_execute_callback execute,
    napi_async_complete_callback complete, void* data, napi_async_work* result) {
  (void)async_resource;
  (void)async_resource_name;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  auto* work = new (std::nothrow) napi_async_work__();
  if (work == nullptr) return napi_generic_failure;
  work->env = env;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  *result = work;
  return *result != nullptr ? NapiQuickjsClearLastError(env) : napi_generic_failure;
}

napi_status NAPI_CDECL napi_delete_async_work(napi_env env, napi_async_work work) {
  if (env == nullptr || work == nullptr) return NapiQuickjsInvalidArg(env);
  delete work;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env env, napi_async_work work) {
  if (env == nullptr || work == nullptr) return napi_invalid_arg;
  if (work->queued) {
    return NapiQuickjsSetLastError(reinterpret_cast<napi_env>(env),
                                   napi_invalid_arg,
                                   "Async work has already been queued");
  }
  work->queued = true;
  if (!work->cancelled && work->execute != nullptr) {
    work->execute(reinterpret_cast<napi_env>(env), work->data);
  }
  if (work->complete != nullptr) {
    work->complete(reinterpret_cast<napi_env>(env),
                   work->cancelled ? napi_cancelled : napi_ok,
                   work->data);
  }
  return NapiQuickjsClearLastError(reinterpret_cast<napi_env>(env));
}

napi_status NAPI_CDECL napi_cancel_async_work(node_api_basic_env env, napi_async_work work) {
  if (env == nullptr || work == nullptr) return napi_invalid_arg;
  work->cancelled = true;
  return NapiQuickjsClearLastError(reinterpret_cast<napi_env>(env));
}

napi_status NAPI_CDECL napi_get_node_version(node_api_basic_env env, const napi_node_version** version) {
  if (env == nullptr || version == nullptr) return NapiQuickjsInvalidArg(env);
  static const napi_node_version node_version = {22, 0, 0, "quickjs-skeleton"};
  *version = &node_version;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s** loop) {
  if (env == nullptr || loop == nullptr) return NapiQuickjsInvalidArg(env);
  *loop = nullptr;
  return NapiQuickjsUnsupported(env, "QuickJS provider does not expose a uv loop yet");
}

napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  return napi_throw(env, err);
}

napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun, void* arg) {
  if (env == nullptr || fun == nullptr) return NapiQuickjsInvalidArg(env);
  env->cleanup_hooks.push_back(fun);
  env->cleanup_hook_args.push_back(arg);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun, void* arg) {
  if (env == nullptr || fun == nullptr) return NapiQuickjsInvalidArg(env);
  for (size_t i = 0; i < env->cleanup_hooks.size(); ++i) {
    if (env->cleanup_hooks[i] == fun && env->cleanup_hook_args[i] == arg) {
      env->cleanup_hooks.erase(env->cleanup_hooks.begin() + static_cast<long>(i));
      env->cleanup_hook_args.erase(env->cleanup_hook_args.begin() + static_cast<long>(i));
      break;
    }
  }
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_open_callback_scope(
    napi_env env, napi_value resource_object, napi_async_context context, napi_callback_scope* result) {
  (void)resource_object;
  (void)context;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = reinterpret_cast<napi_callback_scope>(new (std::nothrow) int(0));
  return *result != nullptr ? NapiQuickjsClearLastError(env) : napi_generic_failure;
}

napi_status NAPI_CDECL napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  if (env == nullptr || scope == nullptr) return NapiQuickjsInvalidArg(env);
  delete reinterpret_cast<int*>(scope);
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource, napi_value async_resource_name, size_t max_queue_size,
    size_t initial_thread_count, void* thread_finalize_data, napi_finalize thread_finalize_cb, void* context,
    napi_threadsafe_function_call_js call_js_cb, napi_threadsafe_function* result) {
  (void)func;
  (void)async_resource;
  (void)async_resource_name;
  (void)max_queue_size;
  (void)initial_thread_count;
  (void)thread_finalize_data;
  (void)thread_finalize_cb;
  (void)context;
  (void)call_js_cb;
  if (env == nullptr || result == nullptr) return NapiQuickjsInvalidArg(env);
  *result = reinterpret_cast<napi_threadsafe_function>(new (std::nothrow) int(0));
  return *result != nullptr ? NapiQuickjsClearLastError(env) : napi_generic_failure;
}

napi_status NAPI_CDECL napi_get_threadsafe_function_context(napi_threadsafe_function func, void** result) {
  if (func == nullptr || result == nullptr) return napi_invalid_arg;
  *result = nullptr;
  return napi_ok;
}

napi_status NAPI_CDECL napi_call_threadsafe_function(
    napi_threadsafe_function func, void* data, napi_threadsafe_function_call_mode is_blocking) {
  (void)data;
  (void)is_blocking;
  return func != nullptr ? napi_ok : napi_invalid_arg;
}

napi_status NAPI_CDECL napi_acquire_threadsafe_function(napi_threadsafe_function func) {
  return func != nullptr ? napi_ok : napi_invalid_arg;
}

napi_status NAPI_CDECL napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  (void)mode;
  if (func == nullptr) return napi_invalid_arg;
  delete reinterpret_cast<int*>(func);
  return napi_ok;
}

napi_status NAPI_CDECL napi_unref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function func) {
  (void)func;
  return env != nullptr ? napi_ok : napi_invalid_arg;
}

napi_status NAPI_CDECL napi_ref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function func) {
  (void)func;
  return env != nullptr ? napi_ok : napi_invalid_arg;
}

napi_status NAPI_CDECL napi_add_async_cleanup_hook(
    node_api_basic_env env, napi_async_cleanup_hook hook, void* arg, napi_async_cleanup_hook_handle* remove_handle) {
  if (env == nullptr || hook == nullptr) return NapiQuickjsInvalidArg(env);
  auto* handle = new (std::nothrow) napi_async_cleanup_hook_handle__();
  if (handle == nullptr) return napi_generic_failure;
  handle->env = env;
  handle->hook = hook;
  handle->arg = arg;
  env->async_cleanup_hooks.push_back(handle);
  if (remove_handle != nullptr) *remove_handle = handle;
  return NapiQuickjsClearLastError(env);
}

napi_status NAPI_CDECL napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr || remove_handle->env == nullptr) return napi_invalid_arg;
  napi_env env = remove_handle->env;
  auto it = std::find(env->async_cleanup_hooks.begin(), env->async_cleanup_hooks.end(), remove_handle);
  if (it != env->async_cleanup_hooks.end()) env->async_cleanup_hooks.erase(it);
  remove_handle->removed = true;
  delete remove_handle;
  return napi_ok;
}

EXTERN_C_END
