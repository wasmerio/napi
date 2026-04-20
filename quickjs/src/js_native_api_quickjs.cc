#include "internal/quickjs_env.h"
#include <cstring>
#include <cmath>

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

napi_env__::napi_env__(JSContext *context, int32_t module_api_version)
    : ctx{context},
      last_exception{JS_UNINITIALIZED},
      module_api_version(module_api_version)
{
  // TODO: We might needs some of that
  // isolate->SetHostImportModuleDynamicallyCallback(NapiHostImportModuleDynamically);
  // v8::Local<v8::Private> wrapKey = v8::Private::ForApi(
  //     isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_wrap"));
  // wrap_private_key.Reset(isolate, wrapKey);
  // v8::Local<v8::Private> wrapRefKey = v8::Private::ForApi(
  //     isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_wrap_ref"));
  // wrap_ref_private_key.Reset(isolate, wrapRefKey);
  // v8::Local<v8::Private> wrapFinalizeKey = v8::Private::ForApi(
  //     isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_wrap_finalize"));
  // wrap_finalizer_private_key.Reset(isolate, wrapFinalizeKey);
  // v8::Local<v8::Private> bufferKey = v8::Private::ForApi(
  //     isolate, v8::String::NewFromUtf8Literal(isolate, "__napi_buffer_record"));
  // buffer_private_key.Reset(isolate, bufferKey);
  napi_quickjs_clear_last_error(this);
}

napi_env__::~napi_env__()
{
  // TODO: Need to implement cleanup
  // RunEnvCleanupHooks(this);
  // napi_v8_finalize_buffer_records(this);

  // for (auto* raw_record : wrap_finalizers) {
  //   auto* record = static_cast<WrapFinalizerRecord*>(raw_record);
  //   if (record != nullptr) {
  //     InvokeWrapFinalizer(record);
  //     record->handle.Reset();
  //     delete record;
  //   }
  // }
  // wrap_finalizers.clear();

  // for (auto* raw_tsfn : threadsafe_functions) {
  //   auto* tsfn = static_cast<napi_threadsafe_function__*>(raw_tsfn);
  //   if (tsfn != nullptr && !tsfn->finalized.exchange(true) && tsfn->finalize_cb != nullptr) {
  //     tsfn->finalize_cb(this, tsfn->finalize_data, nullptr);
  //   }
  //   delete tsfn;
  // }
  // threadsafe_functions.clear();

  // if (instance_data_finalize_cb != nullptr) {
  //   instance_data_finalize_cb(this, instance_data, instance_data_finalize_hint);
  // }
  // if (env_destroy_callback != nullptr) {
  //   env_destroy_callback(this, env_destroy_callback_data);
  // }
  // edge_environment = nullptr;
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

  napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env,
                                                  napi_value typedarray,
                                                  napi_typedarray_type *type,
                                                  size_t *length,
                                                  void **data,
                                                  napi_value *arraybuffer,
                                                  size_t *byte_offset)
  {
    if (!CheckValue(env, typedarray))
      return InvalidArg(env);

    JSValue local = napi_quickjs_unwrap_value(typedarray);
    int type_idx = JS_GetTypedArrayType(local);
    if (type_idx < 0)
      return napi_invalid_arg;

    if (type != nullptr)
    {
      *type = static_cast<napi_typedarray_type>(type_idx);
    }

    size_t byte_len;
    size_t offset;
    JSValue abuf = JS_GetTypedArrayBuffer(env->ctx, local, &offset, &byte_len, nullptr);

    if (JS_IsException(abuf))
    {
      return ReturnPendingIfCaught(env, "Failed to get typed array info");
    }

    if (length != nullptr)
    {
      // Node-API expects the number of elements, not byte length
      // We need to calculate it based on the element size
      int shift = 0;
      switch (type_idx)
      {
      case napi_int8_array:
      case napi_uint8_array:
      case napi_uint8_clamped_array:
        shift = 0;
        break;
      case napi_int16_array:
      case napi_uint16_array:
      case napi_float16_array:
        shift = 1;
        break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array:
        shift = 2;
        break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array:
        shift = 3;
        break;
      }
      *length = byte_len >> shift;
    }

    if (byte_offset != nullptr)
    {
      *byte_offset = offset;
    }

    if (data != nullptr)
    {
      size_t ab_len;
      uint8_t *ab_data = JS_GetArrayBuffer(env->ctx, &ab_len, abuf);
      if (ab_data == nullptr)
      {
        JS_FreeValue(env->ctx, abuf);
        return ReturnPendingIfCaught(env, "Failed to get array buffer data");
      }
      *data = ab_data + offset;
    }

    if (arraybuffer != nullptr)
    {
      *arraybuffer = napi_quickjs_wrap_value(env, abuf);
      if (*arraybuffer == nullptr)
      {
        JS_FreeValue(env->ctx, abuf);
        return napi_generic_failure;
      }
    }
    else
    {
      JS_FreeValue(env->ctx, abuf);
    }

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer)
  {
    if (!CheckValue(env, arraybuffer))
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(arraybuffer);

    if (!JS_IsArrayBuffer(local))
      return napi_arraybuffer_expected;

    JS_DetachArrayBuffer(env->ctx, local);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env,
                                                      napi_value value,
                                                      bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(value);

    if (!JS_IsArrayBuffer(local))
      return napi_arraybuffer_expected;

    // QuickJS doesn't have a direct public C API to check if an ArrayBuffer is detached.
    // We can check by trying to get its data. If it returns NULL and length 0, it might be detached.
    // However, a cleaner way using the public API is to check its byteLength property.
    // A detached ArrayBuffer has a byteLength of 0.
    JSValue byte_len_val = JS_GetPropertyStr(env->ctx, local, "byteLength");
    if (JS_IsException(byte_len_val))
    {
      return ReturnPendingIfCaught(env, "Failed to check if arraybuffer is detached");
    }

    int64_t len;
    JS_ToInt64(env->ctx, &len, byte_len_val);
    JS_FreeValue(env->ctx, byte_len_val);

    // While a 0-length array buffer exists, checking if it throws on DataView creation
    // is a more robust, albeit slightly hacky, way to test for detachment using only public APIs if byteLength isn't sufficient.
    // Let's use the byteLength == 0 check as a fast path, and if it's 0, we can try to create a TypedArray on it to be sure.
    if (len > 0)
    {
      *result = false;
    }
    else
    {
      // It's either a 0-length buffer or detached. Let's try to get its data pointer.
      // JS_GetArrayBuffer returns NULL if detached (or out of memory, but size will be 0).
      size_t ab_len;
      uint8_t *data = JS_GetArrayBuffer(env->ctx, &ab_len, local);
      if (data == nullptr)
      {
        // It threw an exception (TypeError: ArrayBuffer is detached)
        ClearLastException(env); // Clear the expected exception
        *result = true;
      }
      else
      {
        *result = false;
      }
    }

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_create_array_with_length(napi_env env,
                                                       size_t length,
                                                       napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    JSValue arr = JS_NewArray(env->ctx);
    if (JS_IsException(arr))
    {
      return ReturnPendingIfCaught(env, "Failed to create array");
    }

    if (length > 0)
    {
      JSValue len_val = JS_NewFloat64(env->ctx, static_cast<double>(length));
      if (JS_SetPropertyStr(env->ctx, arr, "length", len_val) < 0)
      {
        JS_FreeValue(env->ctx, arr);
        return ReturnPendingIfCaught(env, "Failed to set array length");
      }
    }

    *result = napi_quickjs_wrap_value(env, arr);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_string_utf8(napi_env env,
                                                 const char *str,
                                                 size_t length,
                                                 napi_value *result)
  {
    if (!CheckEnv(env))
      return napi_invalid_arg;
    if (result == nullptr)
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    if (str == nullptr)
    {
      if (length != 0 && length != NAPI_AUTO_LENGTH)
        return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
      str = "";
      length = 0;
    }

    if (length == NAPI_AUTO_LENGTH)
    {
      length = std::strlen(str);
    }

    JSValue out = JS_NewStringLen(env->ctx, str, length);
    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Cannot create string");
    }

    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_create_string_latin1(napi_env env,
                                                   const char *str,
                                                   size_t length,
                                                   napi_value *result)
  {
    // QuickJS JS_NewStringLen handles UTF-8.
    // For pure Latin-1 (ISO-8859-1), if the string contains characters > 0x7F,
    // they need to be converted to UTF-8 for QuickJS.
    if (!CheckEnv(env))
      return napi_invalid_arg;
    if (result == nullptr)
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    if (str == nullptr)
    {
      if (length != 0 && length != NAPI_AUTO_LENGTH)
        return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
      str = "";
      length = 0;
    }

    if (length == NAPI_AUTO_LENGTH)
    {
      length = std::strlen(str);
    }

    // Check if conversion to UTF-8 is needed
    bool needs_conversion = false;
    for (size_t i = 0; i < length; ++i)
    {
      if (static_cast<unsigned char>(str[i]) > 0x7F)
      {
        needs_conversion = true;
        break;
      }
    }

    JSValue out;
    if (!needs_conversion)
    {
      out = JS_NewStringLen(env->ctx, str, length);
    }
    else
    {
      // Convert Latin1 to UTF-8
      size_t utf8_len = 0;
      for (size_t i = 0; i < length; ++i)
      {
        unsigned char c = str[i];
        utf8_len += (c < 0x80) ? 1 : 2;
      }

      char *utf8_str = static_cast<char *>(js_malloc(env->ctx, utf8_len + 1));
      if (!utf8_str)
        return napi_generic_failure;

      size_t j = 0;
      for (size_t i = 0; i < length; ++i)
      {
        unsigned char c = str[i];
        if (c < 0x80)
        {
          utf8_str[j++] = c;
        }
        else
        {
          utf8_str[j++] = 0xC0 | (c >> 6);
          utf8_str[j++] = 0x80 | (c & 0x3F);
        }
      }
      utf8_str[j] = '\0';
      out = JS_NewStringLen(env->ctx, utf8_str, utf8_len);
      js_free(env->ctx, utf8_str);
    }

    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Cannot create string");
    }

    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_create_string_utf16(napi_env env,
                                                  const char16_t *str,
                                                  size_t length,
                                                  napi_value *result)
  {
    if (!CheckEnv(env))
      return napi_invalid_arg;
    if (result == nullptr)
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    if (str == nullptr)
    {
      if (length != 0 && length != NAPI_AUTO_LENGTH)
        return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
      static const char16_t empty[] = {0};
      str = empty;
      length = 0;
    }

    if (length == NAPI_AUTO_LENGTH)
    {
      const char16_t *p = str;
      while (*p != 0)
        ++p;
      length = static_cast<size_t>(p - str);
    }

    JSValue out = JS_NewStringUTF16(env->ctx, reinterpret_cast<const uint16_t *>(str), length);

    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Cannot create string");
    }

    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_typeof(napi_env env,
                                     napi_value value,
                                     napi_valuetype *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    JSValue local = napi_quickjs_unwrap_value(value);

    if (JS_IsNumber(local))
    {
      *result = napi_number;
    }
    else if (JS_IsString(local))
    {
      *result = napi_string;
    }
    else if (JS_IsBool(local))
    {
      *result = napi_boolean;
    }
    else if (JS_IsNull(local))
    {
      *result = napi_null;
    }
    else if (JS_IsUndefined(local))
    {
      *result = napi_undefined;
    }
    else if (JS_IsSymbol(local))
    {
      *result = napi_symbol;
    }
    else if (JS_IsFunction(env->ctx, local))
    {
      *result = napi_function;
    }
    else if (JS_IsBigInt(local))
    {
      *result = napi_bigint;
    }
    else if (JS_IsObject(local))
    {
      // QuickJS doesn't have a direct "IsExternal" type tag in the same way V8 does.
      // Externals in N-API are usually objects with an opaque pointer.
      // We'd need to check if it has the specific class ID used for externals if we implemented them.
      *result = napi_object;
    }
    else
    {
      // Fallback
      *result = napi_object;
    }

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_double(napi_env env,
                                               napi_value value,
                                               double *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsNumber(local))
    {
      return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
    }
    if (JS_ToFloat64(env->ctx, result, local) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during double coercion");
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_uint32(napi_env env,
                                               napi_value value,
                                               uint32_t *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsNumber(local))
    {
      return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
    }
    if (JS_ToUint32(env->ctx, result, local) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during uint32 coercion");
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_int32(napi_env env,
                                              napi_value value,
                                              int32_t *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsNumber(local))
    {
      return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
    }
    if (JS_ToInt32(env->ctx, result, local) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during int32 coercion");
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_int64(napi_env env,
                                              napi_value value,
                                              int64_t *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsNumber(local))
    {
      return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
    }
    if (JS_ToInt64(env->ctx, result, local) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during int64 coercion");
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_bool(napi_env env,
                                             napi_value value,
                                             bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsBool(local))
    {
      return napi_quickjs_set_last_error(env, napi_boolean_expected, "A boolean was expected");
    }
    // JS_ToBool returns 1 for true, 0 for false, -1 for exception
    int res = JS_ToBool(env->ctx, local);
    if (res < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during bool coercion");
    }
    *result = (res == 1);
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_string_utf8(
      napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result)
  {
    if (!CheckValue(env, value))
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsString(local))
    {
      return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");
    }

    size_t len;
    const char *str = JS_ToCStringLen(env->ctx, &len, local);
    if (!str)
    {
      return ReturnPendingIfCaught(env, "Cannot get string value");
    }

    if (buf == nullptr)
    {
      if (result != nullptr)
      {
        *result = len;
      }
    }
    else if (bufsize != 0)
    {
      size_t copied = std::min(bufsize - 1, len);
      std::memcpy(buf, str, copied);
      buf[copied] = '\0';
      if (result != nullptr)
        *result = copied;
    }
    else if (result != nullptr)
    {
      *result = 0;
    }

    JS_FreeCString(env->ctx, str);
    return napi_quickjs_clear_last_error(env);
  }

  // ... (Other string getters like latin1/utf16 follow a similar pattern,
  //      using JS_ToCStringLen2 or JS_ToCStringLenUTF16) ...

  napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;
    *result = JS_IsArray(napi_quickjs_unwrap_value(value));
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_array_length(napi_env env,
                                               napi_value value,
                                               uint32_t *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;
    JSValue local = napi_quickjs_unwrap_value(value);
    if (!JS_IsArray(local))
      return napi_array_expected;

    JSValue len_val = JS_GetPropertyStr(env->ctx, local, "length");
    if (JS_IsException(len_val))
    {
      return ReturnPendingIfCaught(env, "Exception getting array length");
    }

    if (JS_ToUint32(env->ctx, result, len_val) < 0)
    {
      JS_FreeValue(env->ctx, len_val);
      return ReturnPendingIfCaught(env, "Exception parsing array length");
    }

    JS_FreeValue(env->ctx, len_val);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_element(napi_env env,
                                          napi_value object,
                                          uint32_t index,
                                          napi_value *result)
  {
    if (!CheckValue(env, object) || result == nullptr)
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue out = JS_GetPropertyUint32(env->ctx, local, index);
    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Exception while getting element");
    }
    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_set_element(napi_env env,
                                          napi_value object,
                                          uint32_t index,
                                          napi_value value)
  {
    if (!CheckValue(env, object) || value == nullptr)
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    // JS_SetPropertyUint32 consumes the value, so we must duplicate it
    if (JS_SetPropertyUint32(env->ctx, local, index, JS_DupValue(env->ctx, napi_quickjs_unwrap_value(value))) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while setting element");
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_has_element(napi_env env,
                                          napi_value object,
                                          uint32_t index,
                                          bool *result)
  {
    if (!CheckValue(env, object) || result == nullptr)
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    // QuickJS doesn't have a JS_HasPropertyUint32, so we convert index to atom
    JSAtom prop = JS_NewAtomUInt32(env->ctx, index);
    if (prop == JS_ATOM_NULL)
      return napi_generic_failure;

    int has = JS_HasProperty(env->ctx, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (has < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while checking element");
    }
    *result = (has != 0);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_delete_element(napi_env env,
                                             napi_value object,
                                             uint32_t index,
                                             bool *result)
  {
    if (!CheckValue(env, object))
      return InvalidArg(env);
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_NewAtomUInt32(env->ctx, index);
    if (prop == JS_ATOM_NULL)
      return napi_generic_failure;

    int deleted = JS_DeleteProperty(env->ctx, local, prop, 0);
    JS_FreeAtom(env->ctx, prop);

    if (deleted < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while deleting element");
    }
    if (result != nullptr)
    {
      *result = (deleted != 0);
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_has_named_property(napi_env env,
                                                 napi_value object,
                                                 const char *utf8name,
                                                 bool *result)
  {
    if (!CheckValue(env, object) || utf8name == nullptr || result == nullptr)
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_NewAtom(env->ctx, utf8name);
    if (prop == JS_ATOM_NULL)
      return napi_generic_failure;

    int has = JS_HasProperty(env->ctx, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (has < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while checking named property");
    }
    *result = (has != 0);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_set_named_property(napi_env env,
                                                 napi_value object,
                                                 const char *utf8name,
                                                 napi_value value)
  {
    if (!CheckValue(env, object) || utf8name == nullptr || !CheckValue(env, value))
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    // JS_SetPropertyStr consumes the value, so we duplicate
    if (JS_SetPropertyStr(env->ctx, local, utf8name, JS_DupValue(env->ctx, napi_quickjs_unwrap_value(value))) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while setting named property");
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_named_property(napi_env env,
                                                 napi_value object,
                                                 const char *utf8name,
                                                 napi_value *result)
  {
    if (!CheckValue(env, object) || utf8name == nullptr || result == nullptr)
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue prop = JS_GetPropertyStr(env->ctx, local, utf8name);
    if (JS_IsException(prop))
    {
      return ReturnPendingIfCaught(env, "Exception while getting named property");
    }
    *result = napi_quickjs_wrap_value(env, prop);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_set_property(napi_env env,
                                           napi_value object,
                                           napi_value key,
                                           napi_value value)
  {
    if (!CheckValue(env, object) || !CheckValue(env, key) || !CheckValue(env, value))
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_ValueToAtom(env->ctx, napi_quickjs_unwrap_value(key));
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    // JS_SetProperty consumes value
    int res = JS_SetProperty(env->ctx, local, prop, JS_DupValue(env->ctx, napi_quickjs_unwrap_value(value)));
    JS_FreeAtom(env->ctx, prop);

    if (res < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while setting property");
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_property(napi_env env,
                                           napi_value object,
                                           napi_value key,
                                           napi_value *result)
  {
    if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr)
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_ValueToAtom(env->ctx, napi_quickjs_unwrap_value(key));
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    JSValue out = JS_GetProperty(env->ctx, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Exception while getting property");
    }
    *result = napi_quickjs_wrap_value(env, out);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_has_property(napi_env env,
                                           napi_value object,
                                           napi_value key,
                                           bool *result)
  {
    if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr)
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_ValueToAtom(env->ctx, napi_quickjs_unwrap_value(key));
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    int has = JS_HasProperty(env->ctx, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (has < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while checking property");
    }
    *result = (has != 0);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_delete_property(napi_env env,
                                              napi_value object,
                                              napi_value key,
                                              bool *result)
  {
    if (!CheckValue(env, object) || !CheckValue(env, key))
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_ValueToAtom(env->ctx, napi_quickjs_unwrap_value(key));
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    int deleted = JS_DeleteProperty(env->ctx, local, prop, 0);
    JS_FreeAtom(env->ctx, prop);

    if (deleted < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while deleting property");
    }
    if (result != nullptr)
    {
      *result = (deleted != 0);
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_has_own_property(napi_env env,
                                               napi_value object,
                                               napi_value key,
                                               bool *result)
  {
    if (!CheckValue(env, object) || !CheckValue(env, key) || result == nullptr)
    {
      return InvalidArg(env);
    }
    JSValue local = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSAtom prop = JS_ValueToAtom(env->ctx, napi_quickjs_unwrap_value(key));
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    // JS_GetOwnProperty returns 1 if present, 0 if not, -1 on error
    int has = JS_GetOwnProperty(env->ctx, nullptr, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (has < 0)
    {
      return ReturnPendingIfCaught(env, "Exception while checking own property");
    }
    *result = (has != 0);
    return napi_ok;
  }

} // extern "C"