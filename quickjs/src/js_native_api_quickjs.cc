#include "internal/quickjs_env.h"

// typedef void(NAPI_CDECL* napi_finalize)(napi_env env,
//                                         void* finalize_data,
//                                         void* finalize_hint);

struct napi_external_backing_store_hint__
{
  napi_env env = nullptr;
  void *external_data = nullptr;
  node_api_basic_finalize finalize_cb = nullptr;
  void *finalize_hint = nullptr;
};
using napi_external_backing_store_hint = napi_external_backing_store_hint__;

namespace
{

  inline bool CheckEnv(napi_env env)
  {
    return env != nullptr && env->ctx != nullptr;
  }

  inline bool CheckValue(napi_env env, napi_value value)
  {
    return CheckEnv(env) && value != nullptr;
  }

  void ClearLastException(napi_env env)
  {
    if (env == nullptr)
      return;

    if (JS_IsUndefined(env->last_exception))
      return;

    JS_FreeValue(env->ctx, env->last_exception);

    env->last_exception = JS_UNDEFINED;
  }

  void SetLastException(napi_env env, JSValue exception)
  {
    if (env == nullptr)
      return;

    ClearLastException(env);

    env->last_exception = exception;
  }

  inline napi_status ReturnPendingIfCaught(napi_env env, const char *message)
  {
    if (JS_HasException(env->ctx))
    {
      auto exc = JS_GetException(env->ctx);
      SetLastException(env, exc);
      return napi_quickjs_set_last_error(env, napi_pending_exception, message);
    }
    return napi_quickjs_set_last_error(env, napi_generic_failure, message);
  }

  inline napi_status InvalidArg(napi_env env)
  {
    if (CheckEnv(env))
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    return napi_invalid_arg;
  }

  inline JSTypedArrayEnum ToQuickJSArrayType(napi_typedarray_type type)
  {
    switch (type)
    {
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

    case napi_float32_array:
      return JS_TYPED_ARRAY_FLOAT32;

    case napi_float64_array:
      return JS_TYPED_ARRAY_FLOAT64;

    case napi_bigint64_array:
      return JS_TYPED_ARRAY_BIG_INT64;

    case napi_biguint64_array:
      return JS_TYPED_ARRAY_BIG_UINT64;

    case napi_float16_array:
      return JS_TYPED_ARRAY_FLOAT16;
    }
  }

  void FreeArrayBufferData(JSRuntime *rt, void *opaque, void *ptr)
  {
    js_free_rt(rt, ptr);
  }

  void FreeExternalArrayBufferData(JSRuntime *rt, void *opaque, void *ptr)
  {
    (void)ptr;
    auto hint = reinterpret_cast<napi_external_backing_store_hint *>(opaque);
    if (hint == nullptr)
      return;
    if (hint->finalize_cb != nullptr)
    {
      hint->finalize_cb(hint->env, hint->external_data, hint->finalize_hint);
    }
    delete hint;
  }

}

napi_value__::napi_value__(napi_env env, JSValue local)
    : env(env), value(local) {}

napi_value__::~napi_value__() = default;

napi_env__::napi_env__() : last_exception{JS_UNINITIALIZED}
{
}

JSContext *napi_env__::context() const
{
  return ctx;
}

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message)
{
  if (env == nullptr)
    return status;
  env->last_error.error_code = status;
  env->last_error.engine_error_code = 0;
  env->last_error.engine_reserved = nullptr;
  env->last_error_message = (message == nullptr) ? "" : message;
  env->last_error.error_message =
      env->last_error_message.empty() ? nullptr : env->last_error_message.c_str();
  return status;
}

napi_status napi_quickjs_clear_last_error(napi_env env)
{
  return napi_quickjs_set_last_error(env, napi_ok, nullptr);
}

napi_value napi_quickjs_wrap_value(napi_env env, JSValue value)
{
  if (!CheckEnv(env))
    return nullptr;
  return new (std::nothrow) napi_value__(env, value);
}

JSValue napi_quickjs_unwrap_value(napi_value value)
{
  return value->local();
}

extern "C"
{

  napi_status NAPI_CDECL napi_get_last_error_info(
      node_api_basic_env env, const napi_extended_error_info **result)
  {
    if (result == nullptr)
      return napi_invalid_arg;
    auto *napiEnv = const_cast<napi_env>(env);
    if (!CheckEnv(napiEnv))
      return napi_invalid_arg;
    *result = &napiEnv->last_error;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_UNDEFINED);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NULL);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    auto context = env->context();
    *result = napi_quickjs_wrap_value(env, JS_GetGlobalObject(context));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_boolean(napi_env env,
                                          bool value,
                                          napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewBool(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_double(napi_env env,
                                            double value,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewFloat64(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_int32(napi_env env,
                                           int32_t value,
                                           napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewInt32(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_int64(napi_env env,
                                           int64_t value,
                                           napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewInt64(env->ctx, static_cast<double>(value)));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_uint32(napi_env env,
                                            uint32_t value,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewUint32(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env,
                                                  int64_t value,
                                                  napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewBigInt64(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env,
                                                   uint64_t value,
                                                   napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewBigUint64(env->ctx, value));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_bigint_words(napi_env env,
                                                  int sign_bit,
                                                  size_t word_count,
                                                  const uint64_t *words,
                                                  napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if ((sign_bit != 0 && sign_bit != 1) || word_count > static_cast<size_t>(INT_MAX))
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    if (word_count > 0 && words == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    // TODO: QuickJS has js_bigint_new() and js_bigint_extend() defined as private static functions,
    // they are not accessible. Find out best way of handling this issue.
    return napi_quickjs_set_last_error(env, napi_generic_failure, "BigInt creation from words not supported yet");
  }

  napi_status NAPI_CDECL napi_create_date(napi_env env, double time, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    auto out = JS_NewDate(env->ctx, time); // TODO: Confirm that `time` is `epoch_ms`
    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Failed to create date");
    }
    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewObject(env->ctx));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = napi_quickjs_wrap_value(env, JS_NewArray(env->ctx));
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_external(napi_env env,
                                              void *data,
                                              node_api_basic_finalize finalize_cb,
                                              void *finalize_hint,
                                              napi_value *result)
  {
    (void)finalize_cb;
    (void)finalize_hint;
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    // TODO: Figure out how to create JS object with external data and finalizer.

    // 1. Generate new class ID
    // JS_EXTERN JSClassID JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id);

    // 2. Populate class definition
    // typedef struct JSClassDef {
    //     const char *class_name; /* pure ASCII only! */
    //     JSClassFinalizer *finalizer;
    //     JSClassGCMark *gc_mark;
    //     /* if call != NULL, the object is a function. If (flags &
    //        JS_CALL_FLAG_CONSTRUCTOR) != 0, the function is called as a
    //        constructor. In this case, 'this_val' is new.target. A
    //        constructor call only happens if the object constructor bit is
    //        set (see JS_SetConstructorBit()). */
    //     JSClassCall *call;
    //     /* XXX: suppress this indirection ? It is here only to save memory
    //        because only a few classes need these methods */
    //     JSClassExoticMethods *exotic;
    // } JSClassDef;

    // 3. Register class definition
    // JS_EXTERN int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def);

    // 4. Create object `proto_val = null` and `class_id = <registered class>`
    // JSValue JS_NewObjectProtoClass(JSContext *ctx, JSValueConst proto_val,
    //                                JSClassID class_id)

    // Steps 1...3 should be done once at the installation of NAPI, and
    // the `proto_val = null` will result in object without properties,
    // which is what we probably want for opaque external objects?

    return napi_quickjs_set_last_error(env, napi_generic_failure, "External creation not supported yet");
  }

  napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env,
                                                 size_t byte_length,
                                                 void **data,
                                                 napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    if (data == nullptr && byte_length == 0)
      return napi_invalid_arg;

    auto rt = JS_GetRuntime(env->ctx);
    auto buf = js_malloc_rt(rt, byte_length);
    if (buf != nullptr)
    {
      *data = buf;
    }

    auto ab = JS_NewArrayBuffer(env->ctx, reinterpret_cast<uint8_t *>(buf),
                                byte_length,
                                &FreeArrayBufferData, nullptr,
                                true); // TODO: shared or not-shared?
    if (JS_IsException(ab))
    {
      js_free_rt(rt, buf);
      return ReturnPendingIfCaught(env, "Failed to create array buffer");
    }

    *result = napi_quickjs_wrap_value(env, ab);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_external_arraybuffer(
      napi_env env,
      void *external_data,
      size_t byte_length,
      node_api_basic_finalize finalize_cb,
      void *finalize_hint,
      napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    auto rt = JS_GetRuntime(env->ctx);
    JSValue out;
    if (external_data == nullptr && byte_length == 0)
    {
      uint8_t buf = {};
      out = JS_NewArrayBufferCopy(env->ctx, &buf, 1);
      if (JS_IsException(out))
      {
        return ReturnPendingIfCaught(env, "Failed to create detached array");
      }
      JS_DetachArrayBuffer(env->ctx, out);
    }
    else
    {
      if (external_data == nullptr)
        return napi_invalid_arg;

      // TODO: Maybe instead allocate using js_rt_alloc()
      auto hint = new (std::nothrow) napi_external_backing_store_hint__();
      if (hint == nullptr)
        return napi_generic_failure;
      hint->env = env;
      hint->external_data = external_data;
      hint->finalize_cb = finalize_cb;
      hint->finalize_hint = finalize_hint;

      out = JS_NewArrayBuffer(env->ctx, reinterpret_cast<uint8_t *>(external_data),
                              byte_length,
                              &FreeExternalArrayBufferData,
                              hint,
                              true); // TODO: shared or not-shared?

      if (JS_IsException(out))
      {
        delete hint;
        return ReturnPendingIfCaught(env, "Failed to create external array");
      }
    }

    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool *result)
  {
    if (!CheckEnv(env) || value == nullptr || result == nullptr)
      return napi_invalid_arg;
    *result = JS_GetTypedArrayType(napi_quickjs_unwrap_value(value));
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_create_typedarray(napi_env env,
                                                napi_typedarray_type type,
                                                size_t length,
                                                napi_value arraybuffer,
                                                size_t byte_offset,
                                                napi_value *result)
  {
    if (!CheckEnv(env) || arraybuffer == nullptr || result == nullptr)
      return InvalidArg(env);

    JSValue argv[] = {
        napi_quickjs_unwrap_value(arraybuffer),
        JS_NewBigUint64(env->ctx, byte_offset),
        JS_NewBigUint64(env->ctx, length)};

    JSTypedArrayEnum array_type = ToQuickJSArrayType(type);

    JSValue view = JS_NewTypedArray(env->ctx, 3, argv, array_type);

    if (JS_IsException(view))
    {
      return ReturnPendingIfCaught(env, "Failed to create TypedArray");
    }

    *result = napi_quickjs_wrap_value(env, view);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  // napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env,
  //                                                 napi_value typedarray,
  //                                                 napi_typedarray_type* type,
  //                                                 size_t* length,
  //                                                 void** data,
  //                                                 napi_value* arraybuffer,
  //                                                 size_t* byte_offset) {
  //   if (!CheckEnv(env) || typedarray == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(typedarray);
  //   if (!local->IsTypedArray()) return napi_invalid_arg;
  //   quickjs::Local<quickjs::TypedArray> ta = local.As<quickjs::TypedArray>();

  //   if (type != nullptr && !GetTypedArrayType(local, type)) return napi_generic_failure;
  //   if (length != nullptr) *length = ta->Length();
  //   if (byte_offset != nullptr) *byte_offset = ta->ByteOffset();
  //   if (data != nullptr) {
  //     size_t offset = ta->ByteOffset();
  //     void* buffer_data = ta->Buffer()->Data();
  //     *data = (buffer_data == nullptr) ? nullptr : static_cast<void*>(static_cast<uint8_t*>(buffer_data) + offset);
  //   }
  //   if (arraybuffer != nullptr) {
  //     *arraybuffer = napi_quickjs_wrap_value(env, ta->Buffer());
  //     if (*arraybuffer == nullptr) return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> value = napi_quickjs_unwrap_value(arraybuffer);
  //   if (!value->IsArrayBuffer()) return napi_arraybuffer_expected;
  //   if (value.As<quickjs::ArrayBuffer>()->Detach(quickjs::Local<quickjs::Value>()).FromMaybe(false)) {
  //     return napi_ok;
  //   }
  //   return napi_generic_failure;
  // }

  // napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env,
  //                                                     napi_value value,
  //                                                     bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsArrayBuffer()) return napi_arraybuffer_expected;
  //   *result = local.As<quickjs::ArrayBuffer>()->WasDetached();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_array_with_length(napi_env env,
  //                                                      size_t length,
  //                                                      napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   auto context = env->context();
  //   uint32_t length32 = static_cast<uint32_t>(
  //       length > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
  //           ? std::numeric_limits<uint32_t>::max()
  //           : length);
  //   quickjs::Local<quickjs::Array> arr = quickjs::Array::New(env->ctx);
  //   if (length32 > 0) {
  //     if (!arr
  //              ->Set(context,
  //                    quickjs::String::NewFromUtf8Literal(env->ctx, "length"),
  //                    quickjs::Integer::NewFromUnsigned(env->ctx, length32))
  //              .FromMaybe(false)) {
  //       return napi_generic_failure;
  //     }
  //   }
  //   *result = napi_quickjs_wrap_value(env, arr);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_string_utf8(napi_env env,
  //                                                const char* str,
  //                                                size_t length,
  //                                                napi_value* result) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   if (result == nullptr) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   const int quickjsLength = static_cast<int>(length);
  //   quickjs::MaybeLocal<quickjs::String> maybe =
  //       quickjs::String::NewFromUtf8(env->ctx, str, quickjs::NewStringType::kNormal, quickjsLength);
  //   quickjs::Local<quickjs::String> out;
  //   if (!maybe.ToLocal(&out)) return napi_quickjs_set_last_error(env, napi_generic_failure, "Cannot create string");
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_string_latin1(napi_env env,
  //                                                  const char* str,
  //                                                  size_t length,
  //                                                  napi_value* result) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   if (result == nullptr) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::MaybeLocal<quickjs::String> maybe = quickjs::String::NewFromOneByte(
  //       env->ctx,
  //       reinterpret_cast<const uint8_t*>(str),
  //       quickjs::NewStringType::kNormal,
  //       static_cast<int>(length));
  //   quickjs::Local<quickjs::String> out;
  //   if (!maybe.ToLocal(&out)) return napi_quickjs_set_last_error(env, napi_generic_failure, "Cannot create string");
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_string_utf16(napi_env env,
  //                                                 const char16_t* str,
  //                                                 size_t length,
  //                                                 napi_value* result) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   if (result == nullptr) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     static const char16_t empty[] = {0};
  //     str = empty;
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     const char16_t* p = str;
  //     while (*p != 0) ++p;
  //     length = static_cast<size_t>(p - str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::MaybeLocal<quickjs::String> maybe = quickjs::String::NewFromTwoByte(
  //       env->ctx,
  //       reinterpret_cast<const uint16_t*>(str),
  //       quickjs::NewStringType::kNormal,
  //       static_cast<int>(length));
  //   quickjs::Local<quickjs::String> out;
  //   if (!maybe.ToLocal(&out)) return napi_quickjs_set_last_error(env, napi_generic_failure, "Cannot create string");
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL node_api_create_external_string_latin1(
  //     napi_env env,
  //     char* str,
  //     size_t length,
  //     node_api_basic_finalize finalize_callback,
  //     void* finalize_hint,
  //     napi_value* result,
  //     bool* copied) {
  //   (void)finalize_callback;
  //   (void)finalize_hint;
  //   if (copied != nullptr) *copied = false;
  //   return napi_create_string_latin1(env, str, length, result);
  // }

  // napi_status NAPI_CDECL node_api_create_external_string_utf16(
  //     napi_env env,
  //     char16_t* str,
  //     size_t length,
  //     node_api_basic_finalize finalize_callback,
  //     void* finalize_hint,
  //     napi_value* result,
  //     bool* copied) {
  //   (void)finalize_callback;
  //   (void)finalize_hint;
  //   if (copied != nullptr) *copied = false;
  //   return napi_create_string_utf16(env, str, length, result);
  // }

  // napi_status NAPI_CDECL node_api_create_property_key_latin1(
  //     napi_env env, const char* str, size_t length, napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::String> out;
  //   if (!quickjs::String::NewFromOneByte(
  //            env->ctx,
  //            reinterpret_cast<const uint8_t*>(str),
  //            quickjs::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL node_api_create_property_key_utf8(
  //     napi_env env, const char* str, size_t length, napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::String> out;
  //   if (!quickjs::String::NewFromUtf8(
  //            env->ctx,
  //            str,
  //            quickjs::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL node_api_create_property_key_utf16(
  //     napi_env env, const char16_t* str, size_t length, napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (str == nullptr) {
  //     if (length != 0) return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     static const char16_t empty[] = {0};
  //     str = empty;
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     const char16_t* p = str;
  //     while (*p != 0) ++p;
  //     length = static_cast<size_t>(p - str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::String> out;
  //   if (!quickjs::String::NewFromTwoByte(
  //            env->ctx,
  //            reinterpret_cast<const uint16_t*>(str),
  //            quickjs::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_symbol(napi_env env,
  //                                           napi_value description,
  //                                           napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> desc_value = quickjs::Undefined(env->ctx);
  //   if (description != nullptr) {
  //     if (!CheckValue(env, description)) return napi_invalid_arg;
  //     desc_value = napi_quickjs_unwrap_value(description);
  //     if (!desc_value->IsString()) return napi_string_expected;
  //   }
  //   quickjs::Local<quickjs::Symbol> sym = quickjs::Symbol::New(
  //       env->ctx, desc_value->IsString() ? desc_value.As<quickjs::String>() : quickjs::Local<quickjs::String>());
  //   *result = napi_quickjs_wrap_value(env, sym);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_symbol_for(napi_env env,
  //                                            const char* utf8description,
  //                                            size_t length,
  //                                            napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (utf8description == nullptr && length > 0) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   const char* desc = (utf8description == nullptr) ? "" : utf8description;
  //   const int quickjs_length = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  //   quickjs::Local<quickjs::String> key;
  //   if (!quickjs::String::NewFromUtf8(env->ctx, desc, quickjs::NewStringType::kNormal, quickjs_length)
  //            .ToLocal(&key)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, quickjs::Symbol::For(env->ctx, key));
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_typeof(napi_env env,
  //                                    napi_value value,
  //                                    napi_valuetype* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   *result = TypeOf(napi_quickjs_unwrap_value(value));
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_value_double(napi_env env,
  //                                              napi_value value,
  //                                              double* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsNumber()) {
  //     return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
  //   }
  //   *result = local.As<quickjs::Number>()->Value();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_uint32(napi_env env,
  //                                              napi_value value,
  //                                              uint32_t* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsNumber()) {
  //     return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
  //   }
  //   *result = local->Uint32Value(env->context()).FromMaybe(0);
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_int32(napi_env env,
  //                                             napi_value value,
  //                                             int32_t* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsNumber()) {
  //     return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
  //   }
  //   *result = local->Int32Value(env->context()).FromMaybe(0);
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_int64(napi_env env,
  //                                             napi_value value,
  //                                             int64_t* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (local->IsInt32()) {
  //     *result = local.As<quickjs::Int32>()->Value();
  //     return napi_quickjs_clear_last_error(env);
  //   }
  //   if (!local->IsNumber()) {
  //     return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
  //   }
  //   // Match Node's behavior: non-finite converts to 0, and finite values
  //   // use quickjs IntegerValue conversion (including out-of-range sentinel values).
  //   double double_value = local.As<quickjs::Number>()->Value();
  //   if (std::isfinite(double_value)) {
  //     quickjs::Local<quickjs::Context> empty_context;
  //     *result = local->IntegerValue(empty_context).FromJust();
  //   } else {
  //     *result = 0;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env,
  //                                                    napi_value value,
  //                                                    int64_t* result,
  //                                                    bool* lossless) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_quickjs_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   *result = local.As<quickjs::BigInt>()->Int64Value(lossless);
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env,
  //                                                     napi_value value,
  //                                                     uint64_t* result,
  //                                                     bool* lossless) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_quickjs_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   *result = local.As<quickjs::BigInt>()->Uint64Value(lossless);
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env,
  //                                                    napi_value value,
  //                                                    int* sign_bit,
  //                                                    size_t* word_count,
  //                                                    uint64_t* words) {
  //   if (!CheckEnv(env) || value == nullptr || word_count == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_quickjs_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   quickjs::Local<quickjs::BigInt> bigint = local.As<quickjs::BigInt>();
  //   int sign = 0;
  //   int wc = static_cast<int>(bigint->WordCount());
  //   if (words == nullptr) {
  //     if (sign_bit != nullptr) {
  //       int tmp_count = wc;
  //       uint64_t dummy_word = 0;
  //       uint64_t* tmp_words = (tmp_count > 0) ? &dummy_word : nullptr;
  //       bigint->ToWordsArray(&sign, &tmp_count, tmp_words);
  //       *sign_bit = sign;
  //     }
  //     *word_count = static_cast<size_t>(wc);
  //     return napi_quickjs_clear_last_error(env);
  //   }
  //   int requested = (*word_count > static_cast<size_t>(INT_MAX))
  //                       ? INT_MAX
  //                       : static_cast<int>(*word_count);
  //   bigint->ToWordsArray(&sign, &requested, words);
  //   if (sign_bit != nullptr) *sign_bit = sign;
  //   *word_count = static_cast<size_t>(requested);
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool* is_date) {
  //   if (!CheckEnv(env) || value == nullptr || is_date == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *is_date = napi_quickjs_unwrap_value(value)->IsDate();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value, double* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsDate()) {
  //     return napi_quickjs_set_last_error(env, napi_date_expected, "A date was expected");
  //   }
  //   *result = local.As<quickjs::Date>()->ValueOf();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_quickjs_unwrap_value(value)->IsArrayBuffer();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env,
  //                                                  napi_value arraybuffer,
  //                                                  void** data,
  //                                                  size_t* byte_length) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> value = napi_quickjs_unwrap_value(arraybuffer);
  //   if (value->IsArrayBuffer()) {
  //     quickjs::Local<quickjs::ArrayBuffer> ab = value.As<quickjs::ArrayBuffer>();
  //     if (data != nullptr) *data = ab->Data();
  //     if (byte_length != nullptr) *byte_length = ab->ByteLength();
  //     return napi_quickjs_clear_last_error(env);
  //   }
  //   if (value->IsSharedArrayBuffer()) {
  //     quickjs::Local<quickjs::SharedArrayBuffer> sab = value.As<quickjs::SharedArrayBuffer>();
  //     if (data != nullptr) *data = sab->Data();
  //     if (byte_length != nullptr) *byte_length = sab->ByteLength();
  //     return napi_quickjs_clear_last_error(env);
  //   }
  //   return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  // }

  // napi_status NAPI_CDECL node_api_is_sharedarraybuffer(node_api_basic_env env,
  //                                                      napi_value value,
  //                                                      bool* result) {
  //   auto* napiEnv = const_cast<napi_env>(env);
  //   if (!CheckEnv(napiEnv) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(napiEnv, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_quickjs_unwrap_value(value)->IsSharedArrayBuffer();
  //   return napi_quickjs_clear_last_error(napiEnv);
  // }

  // napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env,
  //                                                          size_t byte_length,
  //                                                          void** data,
  //                                                          napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::SharedArrayBuffer> sab;
  //   if (!quickjs::SharedArrayBuffer::MaybeNew(env->ctx, byte_length).ToLocal(&sab)) {
  //     return napi_quickjs_set_last_error(env, napi_generic_failure, "Failed to create SharedArrayBuffer");
  //   }
  //   if (data != nullptr) *data = sab->Data();
  //   *result = napi_quickjs_wrap_value(env, sab);
  //   if (*result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_generic_failure, "Failed to create SharedArrayBuffer");
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_dataview(napi_env env,
  //                                             size_t length,
  //                                             napi_value arraybuffer,
  //                                             size_t byte_offset,
  //                                             napi_value* result) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> ab = napi_quickjs_unwrap_value(arraybuffer);
  //   if (!ab->IsArrayBuffer() && !ab->IsSharedArrayBuffer()) {
  //     return napi_quickjs_set_last_error(env, napi_arraybuffer_expected, "ArrayBuffer expected");
  //   }

  //   quickjs::TryCatch tc(env->ctx);
  //   auto context = env->context();
  //   quickjs::Local<quickjs::String> ctor_name = quickjs::String::NewFromUtf8Literal(env->ctx, "DataView");
  //   quickjs::Local<quickjs::Value> ctor_val;
  //   if (!context->Global()->Get(context, ctor_name).ToLocal(&ctor_val) || !ctor_val->IsFunction()) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Function> ctor = ctor_val.As<quickjs::Function>();
  //   quickjs::Local<quickjs::Value> args[3] = {
  //       ab,
  //       quickjs::Integer::NewFromUnsigned(env->ctx, static_cast<uint32_t>(byte_offset)),
  //       quickjs::Integer::NewFromUnsigned(env->ctx, static_cast<uint32_t>(length)),
  //   };
  //   quickjs::Local<quickjs::Object> out;
  //   if (!ctor->NewInstance(context, 3, args).ToLocal(&out)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_quickjs_set_last_error(env, napi_pending_exception, "DataView construction threw");
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_quickjs_unwrap_value(value)->IsDataView();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_dataview_info(napi_env env,
  //                                               napi_value dataview,
  //                                               size_t* byte_length,
  //                                               void** data,
  //                                               napi_value* arraybuffer,
  //                                               size_t* byte_offset) {
  //   if (!CheckEnv(env) || dataview == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> view_val = napi_quickjs_unwrap_value(dataview);
  //   if (!view_val->IsDataView()) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::DataView> view = view_val.As<quickjs::DataView>();
  //   if (byte_length != nullptr) *byte_length = view->ByteLength();
  //   if (byte_offset != nullptr) *byte_offset = view->ByteOffset();
  //   if (data != nullptr) {
  //     const size_t offset = view->ByteOffset();
  //     void* buffer_data = view->Buffer()->Data();
  //     *data = (buffer_data == nullptr) ? nullptr
  //                                      : static_cast<void*>(static_cast<uint8_t*>(buffer_data) + offset);
  //   }
  //   if (arraybuffer != nullptr) {
  //     *arraybuffer = napi_quickjs_wrap_value(env, view->Buffer());
  //     if (*arraybuffer == nullptr) return napi_generic_failure;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   *result = napi_quickjs_unwrap_value(value)->IsArray();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_array_length(napi_env env,
  //                                              napi_value value,
  //                                              uint32_t* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsArray()) return napi_array_expected;
  //   *result = local.As<quickjs::Array>()->Length();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_element(napi_env env,
  //                                         napi_value object,
  //                                         uint32_t index,
  //                                         napi_value* result) {
  //   if (!CheckValue(env, object) || result == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(object);
  //   if (!local->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Value> out;
  //   if (!local.As<quickjs::Object>()->Get(env->context(), index).ToLocal(&out)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while getting element");
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_set_element(napi_env env,
  //                                         napi_value object,
  //                                         uint32_t index,
  //                                         napi_value value) {
  //   if (!CheckValue(env, object) || value == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(object);
  //   if (!local->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   if (!local.As<quickjs::Object>()
  //            ->Set(env->context(), index, napi_quickjs_unwrap_value(value))
  //            .FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while setting element");
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_instanceof(napi_env env,
  //                                        napi_value object,
  //                                        napi_value constructor,
  //                                        bool* result) {
  //   if (!CheckValue(env, object) || !CheckValue(env, constructor) || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   quickjs::Local<quickjs::Value> ctor = napi_quickjs_unwrap_value(constructor);
  //   if (!ctor->IsFunction()) return napi_function_expected;
  //   *result = napi_quickjs_unwrap_value(object)
  //                 ->InstanceOf(env->context(), ctor.As<quickjs::Object>())
  //                 .FromMaybe(false);
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_has_element(napi_env env,
  //                                         napi_value object,
  //                                         uint32_t index,
  //                                         bool* result) {
  //   if (!CheckValue(env, object) || result == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(object);
  //   if (!local->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   auto has = local.As<quickjs::Object>()->Has(env->context(), index);
  //   if (has.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while checking element");
  //   }
  //   *result = has.FromJust();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_delete_element(napi_env env,
  //                                            napi_value object,
  //                                            uint32_t index,
  //                                            bool* result) {
  //   if (!CheckValue(env, object)) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(object);
  //   if (!local->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   auto deleted = local.As<quickjs::Object>()->Delete(env->context(), index);
  //   if (deleted.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while deleting element");
  //   }
  //   if (result != nullptr) {
  //     *result = deleted.FromJust();
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_cb_info(napi_env env,
  //                                         napi_callback_info cbinfo,
  //                                         size_t* argc,
  //                                         napi_value* argv,
  //                                         napi_value* this_arg,
  //                                         void** data) {
  //   if (!CheckEnv(env) || cbinfo == nullptr) return napi_invalid_arg;
  //   size_t provided = (argc == nullptr) ? 0 : *argc;
  //   if (argc != nullptr) {
  //     *argc = cbinfo->args.size();
  //   }
  //   if (argv != nullptr && provided > 0) {
  //     const size_t n = (provided < cbinfo->args.size()) ? provided : cbinfo->args.size();
  //     for (size_t i = 0; i < n; ++i) argv[i] = cbinfo->args[i];
  //     for (size_t i = n; i < provided; ++i) {
  //       argv[i] = napi_quickjs_wrap_value(env, quickjs::Undefined(env->ctx));
  //     }
  //   }
  //   if (this_arg != nullptr) *this_arg = cbinfo->this_arg;
  //   if (data != nullptr) *data = cbinfo->data;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_new_target(
  //     napi_env env, napi_callback_info cbinfo, napi_value* result) {
  //   if (!CheckEnv(env) || cbinfo == nullptr || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   *result = cbinfo->new_target;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   auto* scope = new (std::nothrow) napi_handle_scope__();
  //   if (scope == nullptr) return napi_generic_failure;
  //   scope->env = env;
  //   *result = scope;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  //   if (!CheckEnv(env) || scope == nullptr) return napi_invalid_arg;
  //   delete scope;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_open_escapable_handle_scope(
  //     napi_env env, napi_escapable_handle_scope* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   auto* scope = new (std::nothrow) napi_escapable_handle_scope__();
  //   if (scope == nullptr) return napi_generic_failure;
  //   scope->env = env;
  //   *result = scope;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_close_escapable_handle_scope(
  //     napi_env env, napi_escapable_handle_scope scope) {
  //   if (!CheckEnv(env) || scope == nullptr) return napi_invalid_arg;
  //   delete scope;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_escape_handle(napi_env env,
  //                                           napi_escapable_handle_scope scope,
  //                                           napi_value escapee,
  //                                           napi_value* result) {
  //   if (!CheckEnv(env) || scope == nullptr || escapee == nullptr || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   if (scope->escaped) return napi_escape_called_twice;
  //   scope->escaped = true;
  //   *result = escapee;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_function(napi_env env,
  //                                             const char* utf8name,
  //                                             size_t length,
  //                                             napi_callback cb,
  //                                             void* data,
  //                                             napi_value* result) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   if (cb == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   auto* payload = new (std::nothrow) CallbackPayload{env, cb, data};
  //   if (payload == nullptr) return napi_generic_failure;

  //   quickjs::Local<quickjs::External> payloadValue = quickjs::External::New(env->ctx, payload);
  //   quickjs::Local<quickjs::Context> context = env->context();

  //   quickjs::Local<quickjs::String> name;
  //   if (utf8name != nullptr) {
  //     const int quickjsLength =
  //         (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  //     quickjs::MaybeLocal<quickjs::String> maybeName =
  //         quickjs::String::NewFromUtf8(env->ctx, utf8name, quickjs::NewStringType::kNormal, quickjsLength);
  //     if (!maybeName.ToLocal(&name)) return napi_generic_failure;
  //   }

  //   quickjs::MaybeLocal<quickjs::Function> maybeFn = quickjs::Function::New(
  //       context, FunctionTrampoline, payloadValue);
  //   quickjs::Local<quickjs::Function> fn;
  //   if (!maybeFn.ToLocal(&fn)) return napi_generic_failure;
  //   if (!name.IsEmpty()) fn->SetName(name);

  //   *result = napi_quickjs_wrap_value(env, fn);
  //   if (*result == nullptr) return napi_generic_failure;
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_define_class(napi_env env,
  //                                          const char* utf8name,
  //                                          size_t length,
  //                                          napi_callback constructor,
  //                                          void* data,
  //                                          size_t property_count,
  //                                          const napi_property_descriptor* properties,
  //                                          napi_value* result) {
  //   if (!CheckEnv(env)) {
  //     return napi_invalid_arg;
  //   }
  //   if (utf8name == nullptr || constructor == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   if (property_count > 0 && properties == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }

  //   quickjs::Local<quickjs::Context> context = env->context();
  //   auto* payload = new (std::nothrow) CallbackPayload{env, constructor, data};
  //   if (payload == nullptr) return napi_generic_failure;

  //   const int quickjs_length = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  //   quickjs::Local<quickjs::String> name;
  //   if (!quickjs::String::NewFromUtf8(env->ctx,
  //                                utf8name,
  //                                quickjs::NewStringType::kNormal,
  //                                quickjs_length)
  //            .ToLocal(&name)) {
  //     return napi_generic_failure;
  //   }

  //   // Use a FunctionTemplate so instances created through napi_define_class()
  //   // are quickjs API objects, matching Node's host-object behavior for wrapped
  //   // internal classes such as JSStream.
  //   quickjs::Local<quickjs::FunctionTemplate> ctor_template =
  //       quickjs::FunctionTemplate::New(env->ctx,
  //                                 FunctionTrampoline,
  //                                 quickjs::External::New(env->ctx, payload));
  //   ctor_template->SetClassName(name);
  //   ctor_template->InstanceTemplate()->SetInternalFieldCount(1);

  //   quickjs::Local<quickjs::Function> ctor;
  //   if (!ctor_template->GetFunction(context).ToLocal(&ctor)) {
  //     return napi_generic_failure;
  //   }

  //   napi_value ctorValue = napi_quickjs_wrap_value(env, ctor);
  //   if (ctorValue == nullptr) return napi_generic_failure;
  //   quickjs::Local<quickjs::Object> proto = ctor->Get(context, quickjs::String::NewFromUtf8Literal(env->ctx, "prototype"))
  //                                      .ToLocalChecked()
  //                                      .As<quickjs::Object>();

  //   for (size_t i = 0; i < property_count; ++i) {
  //     const napi_property_descriptor& desc = properties[i];
  //     napi_status status = napi_ok;
  //     quickjs::Local<quickjs::Name> key;
  //     if (desc.utf8name != nullptr) {
  //       quickjs::Local<quickjs::String> key_str;
  //       if (!quickjs::String::NewFromUtf8(env->ctx, desc.utf8name, quickjs::NewStringType::kNormal)
  //                .ToLocal(&key_str)) {
  //         return napi_generic_failure;
  //       }
  //       key = key_str;
  //     } else if (desc.name != nullptr) {
  //       quickjs::Local<quickjs::Value> name_value = napi_quickjs_unwrap_value(desc.name);
  //       if (!name_value->IsName()) return napi_name_expected;
  //       key = name_value.As<quickjs::Name>();
  //     } else {
  //       return napi_name_expected;
  //     }
  //     quickjs::Local<quickjs::Object> target =
  //         (desc.attributes & napi_static) ? ctor.As<quickjs::Object>() : proto;

  //     if (desc.method != nullptr) {
  //       napi_value fnValue = nullptr;
  //       status = napi_create_function(
  //           env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, &fnValue);
  //       if (status != napi_ok) return status;
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_quickjs_unwrap_value(fnValue),
  //                ToquickjsPropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return napi_generic_failure;
  //       }
  //       continue;
  //     }

  //     if (desc.getter != nullptr || desc.setter != nullptr) {
  //       quickjs::Local<quickjs::Function> getter_fn;
  //       quickjs::Local<quickjs::Function> setter_fn;
  //       if (desc.getter != nullptr) {
  //         napi_value getter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, &getter_value);
  //         if (status != napi_ok) return status;
  //         getter_fn = napi_quickjs_unwrap_value(getter_value).As<quickjs::Function>();
  //       }
  //       if (desc.setter != nullptr) {
  //         napi_value setter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, &setter_value);
  //         if (status != napi_ok) return status;
  //         setter_fn = napi_quickjs_unwrap_value(setter_value).As<quickjs::Function>();
  //       }
  //       target->SetAccessorProperty(
  //           key,
  //           getter_fn,
  //           setter_fn,
  //           ToquickjsPropertyAttributes(desc.attributes, false));
  //       continue;
  //     }

  //     if (desc.value != nullptr) {
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_quickjs_unwrap_value(desc.value),
  //                ToquickjsPropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return napi_generic_failure;
  //       }
  //       continue;
  //     }
  //   }

  //   *result = ctorValue;
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_new_instance(napi_env env,
  //                                          napi_value constructor,
  //                                          size_t argc,
  //                                          const napi_value* argv,
  //                                          napi_value* result) {
  //   if (!CheckValue(env, constructor) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> ctorValue = napi_quickjs_unwrap_value(constructor);
  //   if (!ctorValue->IsFunction()) return napi_function_expected;
  //   quickjs::Local<quickjs::Function> ctor = ctorValue.As<quickjs::Function>();
  //   std::vector<quickjs::Local<quickjs::Value>> args;
  //   args.reserve(argc);
  //   for (size_t i = 0; i < argc; ++i) args.push_back(napi_quickjs_unwrap_value(argv[i]));
  //   quickjs::Local<quickjs::Value> out;
  //   quickjs::TryCatch tryCatch(env->ctx);
  //   if (!ctor->NewInstance(env->context(), static_cast<int>(argc), args.data())
  //            .ToLocal(&out)) {
  //     if (tryCatch.HasCaught()) {
  //       SetLastException(env, tryCatch.Exception(), tryCatch.Message());
  //       return napi_quickjs_set_last_error(env, napi_pending_exception, "Constructor threw");
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_call_function(napi_env env,
  //                                           napi_value recv,
  //                                           napi_value func,
  //                                           size_t argc,
  //                                           const napi_value* argv,
  //                                           napi_value* result) {
  //   if (!CheckValue(env, recv) || !CheckValue(env, func)) return napi_invalid_arg;
  //   auto context = env->context();
  //   quickjs::Local<quickjs::Function> fn;
  //   if (!napi_quickjs_unwrap_value(func)->IsFunction()) return napi_function_expected;
  //   fn = napi_quickjs_unwrap_value(func).As<quickjs::Function>();

  //   std::vector<quickjs::Local<quickjs::Value>> args;
  //   args.reserve(argc);
  //   for (size_t i = 0; i < argc; ++i) {
  //     args.push_back(napi_quickjs_unwrap_value(argv[i]));
  //   }

  //   quickjs::TryCatch tryCatch(env->ctx);
  //   quickjs::MaybeLocal<quickjs::Value> maybe = fn->Call(
  //       context, napi_quickjs_unwrap_value(recv), argc, args.data());
  //   if (tryCatch.HasCaught()) {
  //     SetLastException(env, tryCatch.Exception(), tryCatch.Message());
  //     return napi_quickjs_set_last_error(env, napi_pending_exception, "Function call threw");
  //   }
  //   if (maybe.IsEmpty()) {
  //     return napi_generic_failure;
  //   }
  //   if (result != nullptr) {
  //     quickjs::Local<quickjs::Value> out;
  //     if (!maybe.ToLocal(&out)) return napi_generic_failure;
  //     *result = napi_quickjs_wrap_value(env, out);
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_define_properties(
  //     napi_env env,
  //     napi_value object,
  //     size_t property_count,
  //     const napi_property_descriptor* properties) {
  //   if (!CheckValue(env, object) || properties == nullptr) return InvalidArg(env);
  //   auto context = env->context();
  //   quickjs::Local<quickjs::Value> targetValue = napi_quickjs_unwrap_value(object);
  //   if (!targetValue->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::Object> target = targetValue.As<quickjs::Object>();

  //   quickjs::TryCatch tc(env->ctx);
  //   for (size_t i = 0; i < property_count; ++i) {
  //     const napi_property_descriptor& desc = properties[i];
  //     quickjs::Local<quickjs::Name> key;
  //     if (desc.utf8name != nullptr) {
  //       quickjs::Local<quickjs::String> key_str;
  //       if (!quickjs::String::NewFromUtf8(
  //                env->ctx, desc.utf8name, quickjs::NewStringType::kNormal)
  //                .ToLocal(&key_str)) {
  //         return napi_generic_failure;
  //       }
  //       key = key_str;
  //     } else if (desc.name != nullptr) {
  //       quickjs::Local<quickjs::Value> name_value = napi_quickjs_unwrap_value(desc.name);
  //       if (!name_value->IsName()) return napi_name_expected;
  //       key = name_value.As<quickjs::Name>();
  //     } else {
  //       return napi_name_expected;
  //     }

  //     if (desc.method != nullptr) {
  //       napi_value fnValue = nullptr;
  //       napi_status status = napi_create_function(
  //           env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, &fnValue);
  //       if (status != napi_ok) return status;
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_quickjs_unwrap_value(fnValue),
  //                ToquickjsPropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return ReturnPendingIfCaught(env, tc, "Exception while defining property");
  //       }
  //       continue;
  //     }

  //     if (desc.getter != nullptr || desc.setter != nullptr) {
  //       napi_status status = napi_ok;
  //       quickjs::Local<quickjs::Function> getter_fn;
  //       quickjs::Local<quickjs::Function> setter_fn;
  //       if (desc.getter != nullptr) {
  //         napi_value getter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, &getter_value);
  //         if (status != napi_ok) return status;
  //         getter_fn = napi_quickjs_unwrap_value(getter_value).As<quickjs::Function>();
  //       }
  //       if (desc.setter != nullptr) {
  //         napi_value setter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, &setter_value);
  //         if (status != napi_ok) return status;
  //         setter_fn = napi_quickjs_unwrap_value(setter_value).As<quickjs::Function>();
  //       }
  //       target->SetAccessorProperty(
  //           key,
  //           getter_fn,
  //           setter_fn,
  //           ToquickjsPropertyAttributes(desc.attributes, false));
  //       continue;
  //     }

  //     if (desc.value != nullptr) {
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_quickjs_unwrap_value(desc.value),
  //                ToquickjsPropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return ReturnPendingIfCaught(env, tc, "Exception while defining property");
  //       }
  //     }
  //   }

  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_promise(napi_env env,
  //                                            napi_deferred* deferred,
  //                                            napi_value* promise) {
  //   if (!CheckEnv(env) || deferred == nullptr || promise == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Promise::Resolver> resolver;
  //   if (!quickjs::Promise::Resolver::New(env->context()).ToLocal(&resolver)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to create Promise resolver");
  //   }
  //   auto* d = new (std::nothrow) napi_deferred__();
  //   if (d == nullptr) return napi_generic_failure;
  //   d->env = env;
  //   d->resolver.Reset(env->ctx, resolver);
  //   *deferred = d;
  //   *promise = napi_quickjs_wrap_value(env, resolver->GetPromise());
  //   if (*promise == nullptr) {
  //     delete d;
  //     *deferred = nullptr;
  //     return napi_generic_failure;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
  //                                              napi_deferred deferred,
  //                                              napi_value resolution) {
  //   if (!CheckEnv(env) || deferred == nullptr || resolution == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Promise::Resolver> resolver = deferred->resolver.Get(env->ctx);
  //   if (!resolver->Resolve(env->context(), napi_quickjs_unwrap_value(resolution)).FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to resolve promise");
  //   }
  //   delete deferred;
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
  //                                             napi_deferred deferred,
  //                                             napi_value rejection) {
  //   if (!CheckEnv(env) || deferred == nullptr || rejection == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Promise::Resolver> resolver = deferred->resolver.Get(env->ctx);
  //   if (!resolver->Reject(env->context(), napi_quickjs_unwrap_value(rejection)).FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to reject promise");
  //   }
  //   delete deferred;
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool* is_promise) {
  //   if (!CheckEnv(env) || value == nullptr || is_promise == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *is_promise = napi_quickjs_unwrap_value(value)->IsPromise();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_has_named_property(napi_env env,
  //                                                napi_value object,
  //                                                const char* utf8name,
  //                                                bool* result) {
  //   if (!CheckValue(env, object) || utf8name == nullptr || result == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   auto context = env->context();
  //   quickjs::Local<quickjs::Value> targetValue = napi_quickjs_unwrap_value(object);
  //   if (!targetValue->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::String> key;
  //   if (!quickjs::String::NewFromUtf8(
  //            env->ctx, utf8name, quickjs::NewStringType::kNormal)
  //            .ToLocal(&key)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   auto has = targetValue.As<quickjs::Object>()->Has(context, key);
  //   if (has.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while checking named property");
  //   }
  //   *result = has.FromJust();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_set_property(napi_env env,
  //                                          napi_value object,
  //                                          napi_value key,
  //                                          napi_value value) {
  //   if (!CheckValue(env, object) || !CheckValue(env, key) || value == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   if (!target.As<quickjs::Object>()
  //            ->Set(env->context(), napi_quickjs_unwrap_value(key), napi_quickjs_unwrap_value(value))
  //            .FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while setting property");
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_property(napi_env env,
  //                                          napi_value object,
  //                                          napi_value key,
  //                                          napi_value* result) {
  //   if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Value> out;
  //   if (!target.As<quickjs::Object>()->Get(env->context(), napi_quickjs_unwrap_value(key)).ToLocal(&out)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while getting property");
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_has_property(napi_env env,
  //                                          napi_value object,
  //                                          napi_value key,
  //                                          bool* result) {
  //   if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   auto has = target.As<quickjs::Object>()->Has(env->context(), napi_quickjs_unwrap_value(key));
  //   if (has.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while checking property");
  //   }
  //   *result = has.FromJust();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_delete_property(napi_env env,
  //                                             napi_value object,
  //                                             napi_value key,
  //                                             bool* result) {
  //   if (!CheckValue(env, object) || !CheckValue(env, key)) {
  //     return InvalidArg(env);
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   auto deleted = target.As<quickjs::Object>()->Delete(env->context(), napi_quickjs_unwrap_value(key));
  //   if (deleted.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while deleting property");
  //   }
  //   if (result != nullptr) {
  //     *result = deleted.FromJust();
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_has_own_property(napi_env env,
  //                                              napi_value object,
  //                                              napi_value key,
  //                                              bool* result) {
  //   if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   quickjs::Local<quickjs::Value> key_value = napi_quickjs_unwrap_value(key);
  //   if (!key_value->IsName()) {
  //     return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   auto has = target.As<quickjs::Object>()->HasOwnProperty(env->context(), key_value.As<quickjs::Name>());
  //   if (has.IsNothing()) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while checking own property");
  //   }
  //   *result = has.FromJust();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_property_names(napi_env env,
  //                                                napi_value object,
  //                                                napi_value* result) {
  //   return napi_quickjs_internal::GetPropertyNames(env,
  //                                             object,
  //                                             quickjs::KeyCollectionMode::kIncludePrototypes,
  //                                             napi_key_enumerable | napi_key_skip_symbols,
  //                                             quickjs::IndexFilter::kIncludeIndices,
  //                                             napi_key_numbers_to_strings,
  //                                             "Exception while getting property names",
  //                                             result);
  // }

  // napi_status NAPI_CDECL napi_get_all_property_names(napi_env env,
  //                                                    napi_value object,
  //                                                    napi_key_collection_mode key_mode,
  //                                                    napi_key_filter key_filter,
  //                                                    napi_key_conversion key_conversion,
  //                                                    napi_value* result) {
  //   quickjs::KeyCollectionMode collection_mode =
  //       (key_mode == napi_key_own_only) ? quickjs::KeyCollectionMode::kOwnOnly
  //                                       : quickjs::KeyCollectionMode::kIncludePrototypes;
  //   return napi_quickjs_internal::GetPropertyNames(env,
  //                                             object,
  //                                             collection_mode,
  //                                             static_cast<uint32_t>(key_filter),
  //                                             quickjs::IndexFilter::kIncludeIndices,
  //                                             key_conversion,
  //                                             "Exception while getting all property names",
  //                                             result);
  // }

  // napi_status NAPI_CDECL napi_set_named_property(napi_env env,
  //                                                napi_value object,
  //                                                const char* utf8name,
  //                                                napi_value value) {
  //   if (!CheckValue(env, object) || utf8name == nullptr || value == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   auto context = env->context();
  //   quickjs::Local<quickjs::Value> targetValue = napi_quickjs_unwrap_value(object);
  //   if (!targetValue->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::String> key;
  //   if (!quickjs::String::NewFromUtf8(
  //            env->ctx, utf8name, quickjs::NewStringType::kNormal)
  //            .ToLocal(&key)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   if (!targetValue.As<quickjs::Object>()
  //            ->Set(context, key, napi_quickjs_unwrap_value(value))
  //            .FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while setting named property");
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_named_property(napi_env env,
  //                                                napi_value object,
  //                                                const char* utf8name,
  //                                                napi_value* result) {
  //   if (!CheckValue(env, object) || utf8name == nullptr || result == nullptr) {
  //     return InvalidArg(env);
  //   }
  //   auto context = env->context();
  //   quickjs::Local<quickjs::Value> targetValue = napi_quickjs_unwrap_value(object);
  //   if (!targetValue->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::String> key;
  //   if (!quickjs::String::NewFromUtf8(
  //            env->ctx, utf8name, quickjs::NewStringType::kNormal)
  //            .ToLocal(&key)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Value> prop;
  //   if (!targetValue.As<quickjs::Object>()->Get(context, key).ToLocal(&prop)) {
  //     return ReturnPendingIfCaught(env, tc, "Exception while getting named property");
  //   }
  //   *result = napi_quickjs_wrap_value(env, prop);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_prototype(napi_env env,
  //                                           napi_value object,
  //                                           napi_value* result) {
  //   if (!CheckValue(env, object) || result == nullptr) return InvalidArg(env);
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::Value> proto = target.As<quickjs::Object>()->GetPrototypeV2();
  //   *result = napi_quickjs_wrap_value(env, proto);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_set_prototype(napi_env env,
  //                                               napi_value object,
  //                                               napi_value value) {
  //   if (!CheckValue(env, object) || !CheckValue(env, value)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<quickjs::Object>()
  //            ->SetPrototypeV2(env->context(), napi_quickjs_unwrap_value(value))
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_value_bool(napi_env env,
  //                                            napi_value value,
  //                                            bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsBoolean()) {
  //     return napi_quickjs_set_last_error(env, napi_boolean_expected, "A boolean was expected");
  //   }
  //   *result = local.As<quickjs::Boolean>()->Value();
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_string_utf8(
  //     napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  //   if (!CheckEnv(env) || value == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsString()) {
  //     return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");
  //   }
  //   quickjs::Local<quickjs::String> str = local.As<quickjs::String>();
  //   if (buf == nullptr) {
  //     if (result == nullptr) {
  //       return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     }
  //     *result = str->Utf8LengthV2(env->ctx);
  //   } else if (bufsize != 0) {
  //     size_t copied = str->WriteUtf8V2(env->ctx,
  //                                      buf,
  //                                      bufsize - 1,
  //                                      quickjs::String::WriteFlags::kReplaceInvalidUtf8);
  //     buf[copied] = '\0';
  //     if (result != nullptr) *result = copied;
  //   } else if (result != nullptr) {
  //     *result = 0;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_string_latin1(
  //     napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  //   if (!CheckEnv(env) || value == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsString()) {
  //     return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");
  //   }
  //   quickjs::Local<quickjs::String> str = local.As<quickjs::String>();
  //   if (buf == nullptr) {
  //     if (result == nullptr) {
  //       return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     }
  //     *result = str->Length();
  //   } else if (bufsize != 0) {
  //     uint32_t length = static_cast<uint32_t>(
  //         std::min(bufsize - 1, static_cast<size_t>(str->Length())));
  //     str->WriteOneByteV2(env->ctx,
  //                         0,
  //                         length,
  //                         reinterpret_cast<uint8_t*>(buf),
  //                         quickjs::String::WriteFlags::kNullTerminate);
  //     if (result != nullptr) *result = length;
  //   } else if (result != nullptr) {
  //     *result = 0;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env,
  //                                                    napi_value value,
  //                                                    char16_t* buf,
  //                                                    size_t bufsize,
  //                                                    size_t* result) {
  //   if (!CheckEnv(env) || value == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsString()) {
  //     return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");
  //   }
  //   quickjs::Local<quickjs::String> str = local.As<quickjs::String>();
  //   if (buf == nullptr) {
  //     if (result == nullptr) {
  //       return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     }
  //     *result = str->Length();
  //   } else if (bufsize != 0) {
  //     uint32_t length = static_cast<uint32_t>(
  //         std::min(bufsize - 1, static_cast<size_t>(str->Length())));
  //     str->WriteV2(env->ctx,
  //                  0,
  //                  length,
  //                  reinterpret_cast<uint16_t*>(buf),
  //                  quickjs::String::WriteFlags::kNullTerminate);
  //     if (result != nullptr) *result = length;
  //   } else if (result != nullptr) {
  //     *result = 0;
  //   }
  //   return napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env,
  //                                            napi_value value,
  //                                            napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_quickjs_wrap_value(
  //       env, quickjs::Boolean::New(env->ctx, napi_quickjs_unwrap_value(value)->BooleanValue(env->ctx)));
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_number(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch try_catch(env->ctx);
  //   quickjs::Local<quickjs::Number> out;
  //   if (!napi_quickjs_unwrap_value(value)->ToNumber(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_quickjs_set_last_error(env, napi_pending_exception, "Exception during number coercion");
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_object(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch try_catch(env->ctx);
  //   quickjs::Local<quickjs::Object> out;
  //   if (!napi_quickjs_unwrap_value(value)->ToObject(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_quickjs_set_last_error(env, napi_pending_exception, "Exception during object coercion");
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_string(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   quickjs::TryCatch try_catch(env->ctx);
  //   quickjs::Local<quickjs::String> out;
  //   if (!napi_quickjs_unwrap_value(value)->ToString(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_quickjs_set_last_error(env, napi_pending_exception, "Exception during string coercion");
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_external(napi_env env,
  //                                                napi_value value,
  //                                                void** result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> local = napi_quickjs_unwrap_value(value);
  //   if (!local->IsExternal()) return napi_invalid_arg;
  //   *result = local.As<quickjs::External>()->Value();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_strict_equals(napi_env env,
  //                                           napi_value lhs,
  //                                           napi_value rhs,
  //                                           bool* result) {
  //   if (!CheckValue(env, lhs) || !CheckValue(env, rhs) || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   *result = napi_quickjs_unwrap_value(lhs)->StrictEquals(napi_quickjs_unwrap_value(rhs));
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_reference(napi_env env,
  //                                              napi_value value,
  //                                              uint32_t initial_refcount,
  //                                              napi_ref* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   *result = new (std::nothrow)
  //       napi_ref__(env, napi_quickjs_unwrap_value(value), initial_refcount);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env, napi_ref ref) {
  //   (void)env;
  //   if (ref == nullptr) return napi_invalid_arg;
  //   // If this weak reference is being deleted while a GC pass is active, quickjs may
  //   // still have a queued weak callback for it. Clearing/resetting the handle and
  //   // keeping the bookkeeping object alive avoids a use-after-free.
  //   if (ref->can_be_weak && ref->refcount == 0) {
  //     if (!ref->value.IsEmpty()) {
  //       ref->value.ClearWeak();
  //       ref->value.Reset();
  //     }
  //     return napi_ok;
  //   }
  //   delete ref;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_reference_ref(napi_env env,
  //                                           napi_ref ref,
  //                                           uint32_t* result) {
  //   if (!CheckEnv(env) || ref == nullptr) return napi_invalid_arg;
  //   if (ref->value.IsEmpty()) {
  //     if (result != nullptr) *result = 0;
  //     return napi_ok;
  //   }
  //   ref->refcount++;
  //   if (ref->refcount == 1 && ref->can_be_weak) {
  //     ref->value.ClearWeak();
  //   }
  //   if (result != nullptr) *result = ref->refcount;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_reference_unref(napi_env env,
  //                                             napi_ref ref,
  //                                             uint32_t* result) {
  //   if (!CheckEnv(env) || ref == nullptr) return napi_invalid_arg;
  //   if (!ref->value.IsEmpty() && ref->refcount > 0) {
  //     ref->refcount--;
  //     if (ref->refcount == 0 && ref->can_be_weak) {
  //       ref->value.SetWeak(ref, ReferenceWeakCallback, quickjs::WeakCallbackType::kParameter);
  //     }
  //   }
  //   if (result != nullptr) *result = ref->refcount;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_reference_value(napi_env env,
  //                                                 napi_ref ref,
  //                                                 napi_value* result) {
  //   if (!CheckEnv(env) || ref == nullptr || result == nullptr) return napi_invalid_arg;
  //   if (ref->value.IsEmpty()) {
  //     *result = nullptr;
  //     return napi_ok;
  //   }
  //   *result = napi_quickjs_wrap_value(env, ref->value.Get(env->ctx));
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_wrap(napi_env env,
  //                                  napi_value js_object,
  //                                  void* native_object,
  //                                  node_api_basic_finalize finalize_cb,
  //                                  void* finalize_hint,
  //                                  napi_ref* result) {
  //   if (!CheckValue(env, js_object)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> value = napi_quickjs_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::Object> object = value.As<quickjs::Object>();
  //   quickjs::Local<quickjs::Private> wrapKey = env->wrap_private_key.Get(env->ctx);
  //   quickjs::Local<quickjs::Value> existing;
  //   if (object->GetPrivate(env->context(), wrapKey).ToLocal(&existing) &&
  //       existing->IsExternal()) {
  //     return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   if (!object->SetPrivate(env->context(), wrapKey, quickjs::External::New(env->ctx, native_object))
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Private> wrapFinalizeKey = env->wrap_finalizer_private_key.Get(env->ctx);
  //   if (finalize_cb != nullptr) {
  //     auto* record = new (std::nothrow) WrapFinalizerRecord();
  //     if (record == nullptr) return napi_generic_failure;
  //     record->env = env;
  //     record->native_object = native_object;
  //     record->finalize_cb = finalize_cb;
  //     record->finalize_hint = finalize_hint;
  //     record->handle.Reset(env->ctx, object);
  //     record->handle.SetWeak(record, WrapWeakCallback, quickjs::WeakCallbackType::kParameter);
  //     env->wrap_finalizers.push_back(record);
  //     object
  //         ->SetPrivate(env->context(), wrapFinalizeKey, quickjs::External::New(env->ctx, record))
  //         .FromMaybe(false);
  //   } else {
  //     object->DeletePrivate(env->context(), wrapFinalizeKey).FromMaybe(false);
  //   }
  //   if (result != nullptr) {
  //     napi_status s = napi_create_reference(env, js_object, 0, result);
  //     if (s != napi_ok) return s;
  //     quickjs::Local<quickjs::Private> wrapRefKey = env->wrap_ref_private_key.Get(env->ctx);
  //     object
  //         ->SetPrivate(env->context(), wrapRefKey, quickjs::External::New(env->ctx, *result))
  //         .FromMaybe(false);
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void** result) {
  //   if (!CheckValue(env, js_object) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> value = napi_quickjs_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;
  //   quickjs::Local<quickjs::Object> object = value.As<quickjs::Object>();
  //   quickjs::Local<quickjs::Private> wrapKey = env->wrap_private_key.Get(env->ctx);
  //   quickjs::Local<quickjs::Value> wrapped;
  //   if (!object->GetPrivate(env->context(), wrapKey).ToLocal(&wrapped) ||
  //       !wrapped->IsExternal()) {
  //     return napi_invalid_arg;
  //   }
  //   *result = wrapped.As<quickjs::External>()->Value();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
  //   if (!CheckValue(env, js_object)) return napi_invalid_arg;
  //   void* out = nullptr;
  //   napi_status status = napi_unwrap(env, js_object, &out);
  //   if (status != napi_ok) return status;
  //   quickjs::Local<quickjs::Object> object = napi_quickjs_unwrap_value(js_object).As<quickjs::Object>();
  //   quickjs::Local<quickjs::Private> wrapKey = env->wrap_private_key.Get(env->ctx);
  //   object->DeletePrivate(env->context(), wrapKey).FromMaybe(false);
  //   quickjs::Local<quickjs::Private> wrapRefKey = env->wrap_ref_private_key.Get(env->ctx);
  //   object->DeletePrivate(env->context(), wrapRefKey).FromMaybe(false);
  //   quickjs::Local<quickjs::Private> wrapFinalizeKey = env->wrap_finalizer_private_key.Get(env->ctx);
  //   quickjs::Local<quickjs::Value> finalizeValue;
  //   if (object->GetPrivate(env->context(), wrapFinalizeKey).ToLocal(&finalizeValue) &&
  //       finalizeValue->IsExternal()) {
  //     auto* record = static_cast<WrapFinalizerRecord*>(finalizeValue.As<quickjs::External>()->Value());
  //     if (record != nullptr) {
  //       record->cancelled = true;
  //       record->handle.Reset();
  //       RemoveWrapFinalizerRecord(env, record);
  //       delete record;
  //     }
  //   }
  //   object->DeletePrivate(env->context(), wrapFinalizeKey).FromMaybe(false);
  //   if (result != nullptr) *result = out;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_throw_error(napi_env env,
  //                                         const char* code,
  //                                         const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::String> message;
  //   if (!quickjs::String::NewFromUtf8(
  //            env->ctx, (msg == nullptr) ? "N-API error" : msg, quickjs::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Object> err_obj = quickjs::Exception::Error(message).As<quickjs::Object>();
  //   if (code != nullptr) {
  //     quickjs::Local<quickjs::String> code_key = quickjs::String::NewFromUtf8Literal(env->ctx, "code");
  //     quickjs::Local<quickjs::String> code_val;
  //     if (quickjs::String::NewFromUtf8(env->ctx, code, quickjs::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err_obj->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->ctx->ThrowException(err_obj);
  //   SetLastException(env, err_obj);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  //   if (!CheckValue(env, error)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> ex = napi_quickjs_unwrap_value(error);
  //   env->ctx->ThrowException(ex);
  //   SetLastException(env, ex);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> v = napi_quickjs_unwrap_value(value);
  //   *result = v->IsNativeError();
  //   return napi_ok;
  // }

  // static napi_status CreateErrorCommon(napi_env env,
  //                                      quickjs::Local<quickjs::Value> (*factory)(quickjs::Local<quickjs::String>),
  //                                      napi_value code,
  //                                      napi_value msg,
  //                                      napi_value* result) {
  //   if (!CheckEnv(env) || msg == nullptr || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> msg_val = napi_quickjs_unwrap_value(msg);
  //   if (!msg_val->IsString()) return napi_string_expected;
  //   quickjs::Local<quickjs::String> message = msg_val.As<quickjs::String>();
  //   quickjs::Local<quickjs::Value> created = factory(message);
  //   if (!created->IsObject()) return napi_generic_failure;
  //   quickjs::Local<quickjs::Object> err_obj = created.As<quickjs::Object>();
  //   if (code != nullptr) {
  //     quickjs::Local<quickjs::String> code_key = quickjs::String::NewFromUtf8Literal(env->ctx, "code");
  //     err_obj->Set(env->context(), code_key, napi_quickjs_unwrap_value(code)).FromMaybe(false);
  //   }
  //   *result = napi_quickjs_wrap_value(env, err_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_error(napi_env env,
  //                                          napi_value code,
  //                                          napi_value msg,
  //                                          napi_value* result) {
  //   return CreateErrorCommon(
  //       env,
  //       [](quickjs::Local<quickjs::String> message) { return quickjs::Exception::Error(message); },
  //       code,
  //       msg,
  //       result);
  // }

  // napi_status NAPI_CDECL napi_create_type_error(napi_env env,
  //                                               napi_value code,
  //                                               napi_value msg,
  //                                               napi_value* result) {
  //   return CreateErrorCommon(
  //       env,
  //       [](quickjs::Local<quickjs::String> message) { return quickjs::Exception::TypeError(message); },
  //       code,
  //       msg,
  //       result);
  // }

  // napi_status NAPI_CDECL napi_create_range_error(napi_env env,
  //                                                napi_value code,
  //                                                napi_value msg,
  //                                                napi_value* result) {
  //   return CreateErrorCommon(
  //       env,
  //       [](quickjs::Local<quickjs::String> message) { return quickjs::Exception::RangeError(message); },
  //       code,
  //       msg,
  //       result);
  // }

  // napi_status NAPI_CDECL node_api_create_syntax_error(napi_env env,
  //                                                     napi_value code,
  //                                                     napi_value msg,
  //                                                     napi_value* result) {
  //   return CreateErrorCommon(
  //       env,
  //       [](quickjs::Local<quickjs::String> message) { return quickjs::Exception::SyntaxError(message); },
  //       code,
  //       msg,
  //       result);
  // }

  // napi_status NAPI_CDECL napi_throw_type_error(napi_env env,
  //                                              const char* code,
  //                                              const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::String> message;
  //   if (!quickjs::String::NewFromUtf8(env->ctx,
  //                                (msg == nullptr) ? "Type error" : msg,
  //                                quickjs::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Object> err = quickjs::Exception::TypeError(message).As<quickjs::Object>();
  //   if (code != nullptr) {
  //     quickjs::Local<quickjs::String> code_key = quickjs::String::NewFromUtf8Literal(env->ctx, "code");
  //     quickjs::Local<quickjs::String> code_val;
  //     if (quickjs::String::NewFromUtf8(env->ctx, code, quickjs::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->ctx->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_throw_range_error(napi_env env,
  //                                               const char* code,
  //                                               const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::String> message;
  //   if (!quickjs::String::NewFromUtf8(env->ctx,
  //                                (msg == nullptr) ? "Range error" : msg,
  //                                quickjs::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Object> err = quickjs::Exception::RangeError(message).As<quickjs::Object>();
  //   if (code != nullptr) {
  //     quickjs::Local<quickjs::String> code_key = quickjs::String::NewFromUtf8Literal(env->ctx, "code");
  //     quickjs::Local<quickjs::String> code_val;
  //     if (quickjs::String::NewFromUtf8(env->ctx, code, quickjs::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->ctx->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env,
  //                                                    const char* code,
  //                                                    const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::String> message;
  //   if (!quickjs::String::NewFromUtf8(env->ctx,
  //                                (msg == nullptr) ? "Syntax error" : msg,
  //                                quickjs::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Object> err = quickjs::Exception::SyntaxError(message).As<quickjs::Object>();
  //   if (code != nullptr) {
  //     quickjs::Local<quickjs::String> code_key = quickjs::String::NewFromUtf8Literal(env->ctx, "code");
  //     quickjs::Local<quickjs::String> code_val;
  //     if (quickjs::String::NewFromUtf8(env->ctx, code, quickjs::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->ctx->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   *result = !env->last_exception.IsEmpty();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env,
  //                                                          napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (env->last_exception.IsEmpty()) return napi_generic_failure;
  //   quickjs::Local<quickjs::Value> ex = env->last_exception.Get(env->ctx);
  //   napi_value wrapped = napi_quickjs_wrap_value(env, ex);
  //   if (wrapped != nullptr &&
  //       env->last_exception_source_line.empty() &&
  //       env->last_exception_thrown_at.empty()) {
  //     quickjs::Local<quickjs::Message> message;
  //     if (!env->last_exception_message.IsEmpty()) {
  //       message = env->last_exception_message.Get(env->ctx);
  //     }
  //     if (!message.IsEmpty()) {
  //       const std::string source_line =
  //           unofficial_napi_internal::GetErrorSourceLineForStderrImpl(env, message);
  //       unofficial_napi_internal::SetArrowMessageFromString(
  //           env->ctx, env->context(), ex, source_line);
  //       unofficial_napi_internal::PreserveErrorFormatting(
  //           env,
  //           ex,
  //           source_line,
  //           unofficial_napi_internal::GetThrownAtString(env->ctx, message));
  //     } else {
  //       (void)unofficial_napi_internal::PreserveErrorSourceMessage(env, wrapped);
  //     }
  //   } else {
  //     unofficial_napi_internal::PreserveErrorFormatting(
  //         env,
  //         ex,
  //         env->last_exception_source_line,
  //         env->last_exception_thrown_at);
  //   }
  //   env->last_exception_message.Reset();
  //   ClearLastException(env);
  //   *result = napi_quickjs_wrap_value(env, ex);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env basic_env,
  //                                               void* data,
  //                                               napi_finalize finalize_cb,
  //                                               void* finalize_hint) {
  //   napi_env env = const_cast<napi_env>(basic_env);
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   env->instance_data = data;
  //   env->instance_data_finalize_cb = finalize_cb;
  //   env->instance_data_finalize_hint = finalize_hint;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env basic_env,
  //                                               void** data) {
  //   napi_env env = const_cast<napi_env>(basic_env);
  //   if (!CheckEnv(env) || data == nullptr) return napi_invalid_arg;
  //   *data = env->instance_data;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_run_script(napi_env env,
  //                                        napi_value script,
  //                                        napi_value* result) {
  //   if (!CheckValue(env, script) || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> source = napi_quickjs_unwrap_value(script);
  //   if (!source->IsString()) return napi_string_expected;
  //   quickjs::TryCatch tc(env->ctx);
  //   quickjs::Local<quickjs::Script> compiled;
  //   if (!quickjs::Script::Compile(env->context(), source.As<quickjs::String>()).ToLocal(&compiled)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_pending_exception;
  //     }
  //     return napi_generic_failure;
  //   }
  //   quickjs::Local<quickjs::Value> out;
  //   if (!compiled->Run(env->context()).ToLocal(&out)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_pending_exception;
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_quickjs_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  //   if (!CheckEnv(env) || err == nullptr) return napi_invalid_arg;
  //   SetLastException(env, napi_quickjs_unwrap_value(err));
  //   env->ctx->ThrowException(napi_quickjs_unwrap_value(err));
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env env,
  //                                                  napi_cleanup_hook fun,
  //                                                  void* arg) {
  //   auto* napiEnv = const_cast<napi_env>(env);
  //   if (!CheckEnv(napiEnv) || fun == nullptr) return napi_invalid_arg;
  //   auto* entry = new (std::nothrow) napi_env_cleanup_hook__();
  //   if (entry == nullptr) return napi_generic_failure;
  //   entry->hook = fun;
  //   entry->arg = arg;
  //   napiEnv->env_cleanup_hooks.push_back(entry);
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env env,
  //                                                     napi_cleanup_hook fun,
  //                                                     void* arg) {
  //   auto* napiEnv = const_cast<napi_env>(env);
  //   if (!CheckEnv(napiEnv) || fun == nullptr) return napi_invalid_arg;
  //   auto& hooks = napiEnv->env_cleanup_hooks;
  //   for (auto it = hooks.begin(); it != hooks.end(); ++it) {
  //     auto* entry = static_cast<napi_env_cleanup_hook__*>(*it);
  //     if (entry != nullptr && entry->hook == fun && entry->arg == arg) {
  //       delete entry;
  //       hooks.erase(it);
  //       return napi_ok;
  //     }
  //   }
  //   return napi_invalid_arg;
  // }

  // napi_status NAPI_CDECL napi_create_buffer(napi_env env,
  //                                           size_t length,
  //                                           void** data,
  //                                           napi_value* result) {
  //   if (!CheckEnv(env) || data == nullptr || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Context> context = env->context();
  //   quickjs::Context::Scope context_scope(context);

  //   auto backing = quickjs::ArrayBuffer::NewBackingStore(env->ctx, length);
  //   if (!backing) return napi_generic_failure;
  //   *data = backing->Data();

  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = std::move(backing);

  //   quickjs::Local<quickjs::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  //   record->holder.Reset(env->ctx, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, quickjs::WeakCallbackType::kParameter);
  //   quickjs::Local<quickjs::Private> key = env->buffer_private_key.Get(env->ctx);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, quickjs::External::New(env->ctx, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);

  //   *result = napi_quickjs_wrap_value(env, buffer_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env,
  //                                                size_t length,
  //                                                const void* data,
  //                                                void** result_data,
  //                                                napi_value* result) {
  //   void* out = nullptr;
  //   napi_status status = napi_create_buffer(env, length, &out, result);
  //   if (status != napi_ok) return status;
  //   if (length > 0 && data != nullptr) {
  //     std::memcpy(out, data, length);
  //   }
  //   if (result_data != nullptr) *result_data = out;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_external_buffer(napi_env env,
  //                                                    size_t length,
  //                                                    void* data,
  //                                                    node_api_basic_finalize finalize_cb,
  //                                                    void* finalize_hint,
  //                                                    napi_value* result) {
  //   if (!CheckEnv(env) || data == nullptr || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Context> context = env->context();
  //   quickjs::Context::Scope context_scope(context);

  //   auto* hint = new (std::nothrow) napi_external_backing_store_hint__();
  //   if (hint == nullptr) return napi_generic_failure;
  //   hint->env = env;
  //   hint->external_data = data;
  //   hint->finalize_cb = finalize_cb;
  //   hint->finalize_hint = finalize_hint;

  //   std::unique_ptr<quickjs::BackingStore> backing =
  //       quickjs::ArrayBuffer::NewBackingStore(data, length, ExternalBackingStoreDeleter, hint);
  //   if (!backing) {
  //     delete hint;
  //     return napi_generic_failure;
  //   }

  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = std::move(backing);

  //   quickjs::Local<quickjs::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  //   record->holder.Reset(env->ctx, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, quickjs::WeakCallbackType::kParameter);
  //   quickjs::Local<quickjs::Private> key = env->buffer_private_key.Get(env->ctx);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, quickjs::External::New(env->ctx, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);

  //   *result = napi_quickjs_wrap_value(env, buffer_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> raw = napi_quickjs_unwrap_value(value);
  //   *result = raw->IsArrayBufferView();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
  //                                             napi_value value,
  //                                             void** data,
  //                                             size_t* length) {
  //   if (!CheckEnv(env) || value == nullptr) return napi_invalid_arg;
  //   if (!GetArrayBufferViewInfo(napi_quickjs_unwrap_value(value), data, length)) {
  //     return napi_invalid_arg;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
  //     napi_env env,
  //     napi_value arraybuffer,
  //     size_t byte_offset,
  //     size_t byte_length,
  //     napi_value* result) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr || result == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> raw = napi_quickjs_unwrap_value(arraybuffer);
  //   if (!raw->IsArrayBuffer()) return napi_invalid_arg;
  //   quickjs::Local<quickjs::ArrayBuffer> ab = raw.As<quickjs::ArrayBuffer>();
  //   size_t ab_length = ab->ByteLength();
  //   if (byte_offset > ab_length || byte_length > (ab_length - byte_offset)) {
  //     return napi_invalid_arg;
  //   }

  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = ab->GetBackingStore();

  //   quickjs::Local<quickjs::Object> buffer_obj =
  //       CreateBufferObject(env, record->backing_store, byte_offset, byte_length);
  //   record->holder.Reset(env->ctx, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, quickjs::WeakCallbackType::kParameter);
  //   quickjs::Local<quickjs::Private> key = env->buffer_private_key.Get(env->ctx);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, quickjs::External::New(env->ctx, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);

  //   *result = napi_quickjs_wrap_value(env, buffer_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_adjust_external_memory(
  //     node_api_basic_env basic_env, int64_t change_in_bytes, int64_t* adjusted_value) {
  //   napi_env env = const_cast<napi_env>(basic_env);
  //   if (!CheckEnv(env) || adjusted_value == nullptr) return napi_invalid_arg;
  //   *adjusted_value = env->ctx->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_add_finalizer(napi_env env,
  //                                           napi_value js_object,
  //                                           void* finalize_data,
  //                                           node_api_basic_finalize finalize_cb,
  //                                           void* finalize_hint,
  //                                           napi_ref* result) {
  //   if (!CheckValue(env, js_object) || finalize_cb == nullptr) return napi_invalid_arg;

  //   quickjs::Local<quickjs::Value> value = napi_quickjs_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;

  //   auto* record = new (std::nothrow) WrapFinalizerRecord();
  //   if (record == nullptr) return napi_generic_failure;

  //   record->env = env;
  //   record->native_object = finalize_data;
  //   record->finalize_cb = finalize_cb;
  //   record->finalize_hint = finalize_hint;
  //   record->handle.Reset(env->ctx, value.As<quickjs::Object>());
  //   record->handle.SetWeak(record, WrapWeakCallback, quickjs::WeakCallbackType::kParameter);
  //   env->wrap_finalizers.push_back(record);

  //   if (result != nullptr) {
  //     napi_status status = napi_create_reference(env, js_object, 0, result);
  //     if (status != napi_ok) {
  //       RemoveWrapFinalizerRecord(env, record);
  //       record->handle.Reset();
  //       delete record;
  //       return status;
  //     }
  //   }

  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_version(node_api_basic_env env, uint32_t* result) {
  //   if (result == nullptr) return napi_invalid_arg;
  //   auto* napiEnv = const_cast<napi_env>(env);
  //   if (!CheckEnv(napiEnv)) return napi_invalid_arg;
  //   *result = 10;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  //   if (!CheckValue(env, object)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<quickjs::Object>()
  //            ->SetIntegrityLevel(env->context(), quickjs::IntegrityLevel::kFrozen)
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  //   if (!CheckValue(env, object)) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<quickjs::Object>()
  //            ->SetIntegrityLevel(env->context(), quickjs::IntegrityLevel::kSealed)
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_type_tag_object(
  //     napi_env env, napi_value value, const napi_type_tag* type_tag) {
  //   if (!CheckValue(env, value) || type_tag == nullptr) return napi_invalid_arg;
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(value);
  //   if (!target->IsObject() && !target->IsExternal()) return napi_invalid_arg;

  //   for (auto& entry : env->type_tag_entries) {
  //     if (!entry.value.IsEmpty() && entry.value.Get(env->ctx)->StrictEquals(target)) {
  //       entry.tag = *type_tag;
  //       return napi_ok;
  //     }
  //   }

  //   napi_env__::TypeTagEntry entry;
  //   entry.value.Reset(env->ctx, target);
  //   entry.tag = *type_tag;
  //   env->type_tag_entries.push_back(std::move(entry));
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env,
  //                                                   napi_value value,
  //                                                   const napi_type_tag* type_tag,
  //                                                   bool* result) {
  //   if (!CheckValue(env, value) || type_tag == nullptr || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   quickjs::Local<quickjs::Value> target = napi_quickjs_unwrap_value(value);
  //   if (!target->IsObject() && !target->IsExternal()) {
  //     *result = false;
  //     return napi_ok;
  //   }

  //   for (auto& entry : env->type_tag_entries) {
  //     if (entry.value.IsEmpty()) continue;
  //     if (entry.value.Get(env->ctx)->StrictEquals(target)) {
  //       *result = (entry.tag.lower == type_tag->lower && entry.tag.upper == type_tag->upper);
  //       return napi_ok;
  //     }
  //   }
  //   *result = false;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL
  // node_api_create_object_with_properties(napi_env env,
  //                                        napi_value prototype_or_null,
  //                                        napi_value* property_names,
  //                                        napi_value* property_values,
  //                                        size_t property_count,
  //                                        napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if ((property_count > 0) && (property_names == nullptr || property_values == nullptr)) {
  //     return napi_invalid_arg;
  //   }
  //   quickjs::Local<quickjs::Object> obj = quickjs::Object::New(env->ctx);
  //   if (prototype_or_null != nullptr) {
  //     quickjs::Local<quickjs::Value> proto = napi_quickjs_unwrap_value(prototype_or_null);
  //     if (!proto->IsNull() && !proto->IsObject()) return napi_object_expected;
  //     if (!obj->SetPrototypeV2(env->context(), proto).FromMaybe(false)) {
  //       return napi_generic_failure;
  //     }
  //   }
  //   for (size_t i = 0; i < property_count; ++i) {
  //     if (property_names[i] == nullptr || property_values[i] == nullptr) return napi_invalid_arg;
  //     if (!obj
  //              ->Set(env->context(),
  //                    napi_quickjs_unwrap_value(property_names[i]),
  //                    napi_quickjs_unwrap_value(property_values[i]))
  //              .FromMaybe(false)) {
  //       return napi_generic_failure;
  //     }
  //   }
  //   *result = napi_quickjs_wrap_value(env, obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

} // extern "C"
