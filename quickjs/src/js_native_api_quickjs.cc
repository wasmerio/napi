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

// 1. Declare a Class ID (usually stored in your env or globally)
JSClassID napi_external_class_id = 0;

// 2. Define the QuickJS finalizer function
static void napi_quickjs_external_finalizer(JSRuntime *rt, JSValue val)
{
  // Extract the hint struct we attached to the object
  auto *hint = static_cast<napi_external_backing_store_hint *>(JS_GetOpaque(val, napi_external_class_id));
  if (hint != nullptr)
  {
    // Call the Node-API finalizer callback if the user provided one
    if (hint->finalize_cb != nullptr)
    {
      hint->finalize_cb(hint->env, hint->external_data, hint->finalize_hint);
    }
    // Delete the hint struct itself
    delete hint;
  }
}

// 3. Register the class (Call this ONCE when setting up your JSRuntime)
int RegisterExternalClass(JSRuntime *rt)
{
  JS_NewClassID(rt, &napi_external_class_id);
  JSClassDef def = {};
  def.class_name = "NapiExternal";
  def.finalizer = napi_quickjs_external_finalizer;
  return JS_NewClass(rt, napi_external_class_id, &def);
}

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

JSValue napi_value__::local() const
{
  return value;
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

static JSValue napi_quickjs_create_function_internal(napi_env env,
                                                     const char *utf8name,
                                                     napi_callback cb,
                                                     void *data)
{
  napi_value fn_val;
  napi_status status = napi_create_function(env, utf8name, NAPI_AUTO_LENGTH, cb, data, &fn_val);
  if (status != napi_ok)
    return JS_EXCEPTION;

  return napi_quickjs_unwrap_value(fn_val);
}

static JSValue napi_quickjs_function_bridge(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv,
                                            int magic, JSValue *func_data)
{
  // func_data[0] contains the napi_callback (wrapped in an external)
  // func_data[1] contains the user's data (wrapped in an external)

  // 1. Recover the napi_env from the context (or stored in func_data if preferred)
  // In many implementations, the env is retrievable via JS_GetContextOpaque
  napi_env env = static_cast<napi_env>(JS_GetContextOpaque(ctx));

  // 2. Extract the C++ callback and user data
  void *cb_ptr = nullptr;
  napi_get_value_external(env, napi_quickjs_wrap_value(env, func_data[0]), &cb_ptr);
  napi_callback cb = reinterpret_cast<napi_callback>(cb_ptr);

  void *user_data = nullptr;
  napi_get_value_external(env, napi_quickjs_wrap_value(env, func_data[1]), &user_data);

  // 3. Prepare the info object for the callback
  napi_callback_info__ info = {env, this_val, argc, argv, user_data};

  // 4. Call the actual Node-API callback
  napi_value result = cb(env, reinterpret_cast<napi_callback_info>(&info));

  // 5. Convert return value back to QuickJS
  if (result == nullptr)
    return JS_UNDEFINED;
  return JS_DupValue(ctx, napi_quickjs_unwrap_value(result));
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
                                              napi_finalize finalize_cb,
                                              void *finalize_hint,
                                              napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    // 1. Create a new QuickJS object using our custom external class
    // NOTE: Replace `napi_external_class_id` with how you access it in your engine.
    JSValue obj = JS_NewObjectClass(env->ctx, napi_external_class_id);
    if (JS_IsException(obj))
    {
      return ReturnPendingIfCaught(env, "Failed to create external object");
    }

    // 2. Allocate the hint struct to hold the Node-API finalizer info
    auto *hint = new napi_external_backing_store_hint();
    hint->env = env;
    hint->external_data = data;
    hint->finalize_cb = finalize_cb;
    hint->finalize_hint = finalize_hint;

    // 3. Attach the struct to the QuickJS object
    JS_SetOpaque(obj, hint);

    // 4. Wrap it in a napi_value and return
    *result = napi_quickjs_wrap_value(env, obj);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_external(napi_env env,
                                                 napi_value value,
                                                 void **result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    JSValue local = napi_quickjs_unwrap_value(value);

    // Get the opaque data, ensuring the object is actually of our external class
    auto *hint = static_cast<napi_external_backing_store_hint *>(
        JS_GetOpaque(local, napi_external_class_id));

    if (hint == nullptr)
    {
      return napi_invalid_arg; // Not an external object or opaque data is null
    }

    // Return the original raw C pointer
    *result = hint->external_data;

    return napi_ok;
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
  //     if (length != 0) return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::String> out;
  //   if (!v8::String::NewFromOneByte(
  //            env->isolate,
  //            reinterpret_cast<const uint8_t*>(str),
  //            v8::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL node_api_create_property_key_utf8(
  //     napi_env env, const char* str, size_t length, napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (str == nullptr) {
  //     if (length != 0) return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     str = "";
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     length = std::strlen(str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::String> out;
  //   if (!v8::String::NewFromUtf8(
  //            env->isolate,
  //            str,
  //            v8::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL node_api_create_property_key_utf16(
  //     napi_env env, const char16_t* str, size_t length, napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (str == nullptr) {
  //     if (length != 0) return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     static const char16_t empty[] = {0};
  //     str = empty;
  //   }
  //   if (length == NAPI_AUTO_LENGTH) {
  //     const char16_t* p = str;
  //     while (*p != 0) ++p;
  //     length = static_cast<size_t>(p - str);
  //   }
  //   if (length > static_cast<size_t>(INT_MAX)) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::String> out;
  //   if (!v8::String::NewFromTwoByte(
  //            env->isolate,
  //            reinterpret_cast<const uint16_t*>(str),
  //            v8::NewStringType::kInternalized,
  //            static_cast<int>(length))
  //            .ToLocal(&out)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_symbol(napi_env env,
  //                                           napi_value description,
  //                                           napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> desc_value = v8::Undefined(env->isolate);
  //   if (description != nullptr) {
  //     if (!CheckValue(env, description)) return napi_invalid_arg;
  //     desc_value = napi_v8_unwrap_value(description);
  //     if (!desc_value->IsString()) return napi_string_expected;
  //   }
  //   v8::Local<v8::Symbol> sym = v8::Symbol::New(
  //       env->isolate, desc_value->IsString() ? desc_value.As<v8::String>() : v8::Local<v8::String>());
  //   *result = napi_v8_wrap_value(env, sym);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_symbol_for(napi_env env,
  //                                            const char* utf8description,
  //                                            size_t length,
  //                                            napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (utf8description == nullptr && length > 0) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   const char* desc = (utf8description == nullptr) ? "" : utf8description;
  //   const int v8_length = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  //   v8::Local<v8::String> key;
  //   if (!v8::String::NewFromUtf8(env->isolate, desc, v8::NewStringType::kNormal, v8_length)
  //            .ToLocal(&key)) {
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, v8::Symbol::For(env->isolate, key));
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

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
    else if (JS_IsObject(local))
    {
      // Check if it's our external class
      if (JS_GetOpaque(local, napi_external_class_id) != nullptr)
      {
        *result = napi_external;
      }
      else
      {
        *result = napi_object;
      }
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

  // napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env,
  //                                                    napi_value value,
  //                                                    int64_t* result,
  //                                                    bool* lossless) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_v8_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   *result = local.As<v8::BigInt>()->Int64Value(lossless);
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env,
  //                                                     napi_value value,
  //                                                     uint64_t* result,
  //                                                     bool* lossless) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_v8_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   *result = local.As<v8::BigInt>()->Uint64Value(lossless);
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env,
  //                                                    napi_value value,
  //                                                    int* sign_bit,
  //                                                    size_t* word_count,
  //                                                    uint64_t* words) {
  //   if (!CheckEnv(env) || value == nullptr || word_count == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsBigInt()) {
  //     return napi_v8_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  //   }
  //   v8::Local<v8::BigInt> bigint = local.As<v8::BigInt>();
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
  //     return napi_v8_clear_last_error(env);
  //   }
  //   int requested = (*word_count > static_cast<size_t>(INT_MAX))
  //                       ? INT_MAX
  //                       : static_cast<int>(*word_count);
  //   bigint->ToWordsArray(&sign, &requested, words);
  //   if (sign_bit != nullptr) *sign_bit = sign;
  //   *word_count = static_cast<size_t>(requested);
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool* is_date) {
  //   if (!CheckEnv(env) || value == nullptr || is_date == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *is_date = napi_v8_unwrap_value(value)->IsDate();
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value, double* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsDate()) {
  //     return napi_v8_set_last_error(env, napi_date_expected, "A date was expected");
  //   }
  //   *result = local.As<v8::Date>()->ValueOf();
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_v8_unwrap_value(value)->IsArrayBuffer();
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env,
  //                                                  napi_value arraybuffer,
  //                                                  void** data,
  //                                                  size_t* byte_length) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> value = napi_v8_unwrap_value(arraybuffer);
  //   if (value->IsArrayBuffer()) {
  //     v8::Local<v8::ArrayBuffer> ab = value.As<v8::ArrayBuffer>();
  //     if (data != nullptr) *data = ab->Data();
  //     if (byte_length != nullptr) *byte_length = ab->ByteLength();
  //     return napi_v8_clear_last_error(env);
  //   }
  //   if (value->IsSharedArrayBuffer()) {
  //     v8::Local<v8::SharedArrayBuffer> sab = value.As<v8::SharedArrayBuffer>();
  //     if (data != nullptr) *data = sab->Data();
  //     if (byte_length != nullptr) *byte_length = sab->ByteLength();
  //     return napi_v8_clear_last_error(env);
  //   }
  //   return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  // }

  // napi_status NAPI_CDECL node_api_is_sharedarraybuffer(node_api_basic_env env,
  //                                                      napi_value value,
  //                                                      bool* result) {
  //   auto* napiEnv = const_cast<napi_env>(env);
  //   if (!CheckEnv(napiEnv) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(napiEnv, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_v8_unwrap_value(value)->IsSharedArrayBuffer();
  //   return napi_v8_clear_last_error(napiEnv);
  // }

  // napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env,
  //                                                          size_t byte_length,
  //                                                          void** data,
  //                                                          napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::SharedArrayBuffer> sab;
  //   if (!v8::SharedArrayBuffer::MaybeNew(env->isolate, byte_length).ToLocal(&sab)) {
  //     return napi_v8_set_last_error(env, napi_generic_failure, "Failed to create SharedArrayBuffer");
  //   }
  //   if (data != nullptr) *data = sab->Data();
  //   *result = napi_v8_wrap_value(env, sab);
  //   if (*result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_generic_failure, "Failed to create SharedArrayBuffer");
  //   }
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_create_dataview(napi_env env,
  //                                             size_t length,
  //                                             napi_value arraybuffer,
  //                                             size_t byte_offset,
  //                                             napi_value* result) {
  //   if (!CheckEnv(env) || arraybuffer == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> ab = napi_v8_unwrap_value(arraybuffer);
  //   if (!ab->IsArrayBuffer() && !ab->IsSharedArrayBuffer()) {
  //     return napi_v8_set_last_error(env, napi_arraybuffer_expected, "ArrayBuffer expected");
  //   }
  //   v8::TryCatch tc(env->isolate);
  //   auto context = env->context();
  //   v8::Local<v8::String> ctor_name = v8::String::NewFromUtf8Literal(env->isolate, "DataView");
  //   v8::Local<v8::Value> ctor_val;
  //   if (!context->Global()->Get(context, ctor_name).ToLocal(&ctor_val) || !ctor_val->IsFunction()) {
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Function> ctor = ctor_val.As<v8::Function>();
  //   v8::Local<v8::Value> args[3] = {
  //       ab,
  //       v8::Integer::NewFromUnsigned(env->isolate, static_cast<uint32_t>(byte_offset)),
  //       v8::Integer::NewFromUnsigned(env->isolate, static_cast<uint32_t>(length)),
  //   };
  //   v8::Local<v8::Object> out;
  //   if (!ctor->NewInstance(context, 3, args).ToLocal(&out)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_v8_set_last_error(env, napi_pending_exception, "DataView construction threw");
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_v8_unwrap_value(value)->IsDataView();
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_dataview_info(napi_env env,
  //                                               napi_value dataview,
  //                                               size_t* byte_length,
  //                                               void** data,
  //                                               napi_value* arraybuffer,
  //                                               size_t* byte_offset) {
  //   if (!CheckEnv(env) || dataview == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> view_val = napi_v8_unwrap_value(dataview);
  //   if (!view_val->IsDataView()) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::DataView> view = view_val.As<v8::DataView>();
  //   if (byte_length != nullptr) *byte_length = view->ByteLength();
  //   if (byte_offset != nullptr) *byte_offset = view->ByteOffset();
  //   if (data != nullptr) {
  //     const size_t offset = view->ByteOffset();
  //     void* buffer_data = view->Buffer()->Data();
  //     *data = (buffer_data == nullptr) ? nullptr
  //                                      : static_cast<void*>(static_cast<uint8_t*>(buffer_data) + offset);
  //   }
  //   if (arraybuffer != nullptr) {
  //     *arraybuffer = napi_v8_wrap_value(env, view->Buffer());
  //     if (*arraybuffer == nullptr) return napi_generic_failure;
  //   }
  //   return napi_v8_clear_last_error(env);
  // }

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

  napi_status NAPI_CDECL napi_instanceof(napi_env env,
                                         napi_value object,
                                         napi_value constructor,
                                         bool *result)
  {
    if (!CheckValue(env, object) || !CheckValue(env, constructor) || result == nullptr)
    {
      return InvalidArg(env);
    }

    JSValue val = napi_quickjs_unwrap_value(object);
    JSValue ctor = napi_quickjs_unwrap_value(constructor);

    // Node-API generally expects the constructor to be a function.
    // The commented-out V8 code in your file also performs this check.
    if (!JS_IsFunction(env->ctx, ctor))
    {
      return napi_quickjs_set_last_error(env, napi_function_expected, "A function was expected for the constructor");
    }

    // JS_IsInstanceOf returns 1 (true), 0 (false), or -1 (exception)
    int res = JS_IsInstanceOf(env->ctx, val, ctor);

    if (res < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during instanceof check");
    }

    *result = (res != 0);
    return napi_quickjs_clear_last_error(env);
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

  napi_status NAPI_CDECL napi_get_cb_info(napi_env env,
                                          napi_callback_info cbinfo,
                                          size_t *argc,
                                          napi_value *argv,
                                          napi_value *this_arg,
                                          void **data)
  {
    if (cbinfo == nullptr)
    {
      return InvalidArg(env);
    }

    // Cast the opaque pointer back to our internal structure
    auto *info = reinterpret_cast<napi_callback_info__ *>(cbinfo);

    // 1. Handle Arguments (argv) and Argument Count (argc)
    if (argc != nullptr)
    {
      if (argv != nullptr)
      {
        size_t i = 0;
        // Node-API Rule: Copy up to the size of the provided buffer (*argc)
        // or the actual number of arguments (info->argc).
        size_t count = (*argc < (size_t)info->argc) ? *argc : (size_t)info->argc;

        for (i = 0; i < count; i++)
        {
          // We MUST use JS_DupValue because napi_quickjs_wrap_value creates a
          // napi_value that will call JS_FreeValue when it is destroyed.
          argv[i] = napi_quickjs_wrap_value(env, JS_DupValue(env->ctx, info->argv[i]));
        }

        // Node-API Rule: If the user provided a larger buffer than actual arguments,
        // fill the remaining slots with 'undefined'.
        for (; i < *argc; i++)
        {
          argv[i] = napi_quickjs_wrap_value(env, JS_UNDEFINED);
        }
      }

      // Always update *argc to the actual number of arguments available.
      *argc = (size_t)info->argc;
    }

    // 2. Handle the 'this' argument
    if (this_arg != nullptr)
    {
      *this_arg = napi_quickjs_wrap_value(env, JS_DupValue(env->ctx, info->this_val));
    }

    // 3. Handle the user data pointer
    if (data != nullptr)
    {
      *data = info->data;
    }

    return napi_ok;
  }

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

  napi_status NAPI_CDECL napi_create_function(napi_env env,
                                              const char *utf8name,
                                              size_t length,
                                              napi_callback cb,
                                              void *data,
                                              napi_value *result)
  {
    if (!CheckEnv(env) || cb == nullptr || result == nullptr)
    {
      return InvalidArg(env);
    }

    // 1. Wrap the callback and user data into externals so we can
    // pass them safely as "data" to the QuickJS function.
    napi_value cb_external, data_external;
    napi_create_external(env, reinterpret_cast<void *>(cb), nullptr, nullptr, &cb_external);
    napi_create_external(env, data, nullptr, nullptr, &data_external);

    JSValue data_values[2];
    data_values[0] = napi_quickjs_unwrap_value(cb_external);
    data_values[1] = napi_quickjs_unwrap_value(data_external);

    // 2. Create the C function with data
    // JS_NewCFunctionData allows us to attach 'magic' values to the function object.
    // NOTE: data_values are js_dup() -ed, need to free them
    JSValue fn = JS_NewCFunctionData(env->ctx, napi_quickjs_function_bridge,
                                     0, 0, 2, data_values);

    if (JS_IsException(fn))
    {
      return ReturnPendingIfCaught(env, "Failed to create function");
    }

    // 3. Set the function name if provided
    if (utf8name != nullptr)
    {
      // If length is NAPI_AUTO_LENGTH, QuickJS will handle null-terminated string
      JS_DefinePropertyValueStr(env->ctx, fn, "name",
                                JS_NewStringLen(env->ctx, utf8name,
                                                (length == NAPI_AUTO_LENGTH) ? strlen(utf8name) : length),
                                JS_PROP_CONFIGURABLE);
    }

    *result = napi_quickjs_wrap_value(env, fn);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

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
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   if (property_count > 0 && properties == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Context> context = env->context();
  //   auto* payload = new (std::nothrow) CallbackPayload{env, constructor, data};
  //   if (payload == nullptr) return napi_generic_failure;
  //   const int v8_length = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
  //   v8::Local<v8::String> name;
  //   if (!v8::String::NewFromUtf8(env->isolate,
  //                                utf8name,
  //                                v8::NewStringType::kNormal,
  //                                v8_length)
  //            .ToLocal(&name)) {
  //     return napi_generic_failure;
  //   }
  //   // Use a FunctionTemplate so instances created through napi_define_class()
  //   // are V8 API objects, matching Node's host-object behavior for wrapped
  //   // internal classes such as JSStream.
  //   v8::Local<v8::FunctionTemplate> ctor_template =
  //       v8::FunctionTemplate::New(env->isolate,
  //                                 FunctionTrampoline,
  //                                 v8::External::New(env->isolate, payload));
  //   ctor_template->SetClassName(name);
  //   ctor_template->InstanceTemplate()->SetInternalFieldCount(1);
  //   v8::Local<v8::Function> ctor;
  //   if (!ctor_template->GetFunction(context).ToLocal(&ctor)) {
  //     return napi_generic_failure;
  //   }
  //   napi_value ctorValue = napi_v8_wrap_value(env, ctor);
  //   if (ctorValue == nullptr) return napi_generic_failure;
  //   v8::Local<v8::Object> proto = ctor->Get(context, v8::String::NewFromUtf8Literal(env->isolate, "prototype"))
  //                                      .ToLocalChecked()
  //                                      .As<v8::Object>();
  //   for (size_t i = 0; i < property_count; ++i) {
  //     const napi_property_descriptor& desc = properties[i];
  //     napi_status status = napi_ok;
  //     v8::Local<v8::Name> key;
  //     if (desc.utf8name != nullptr) {
  //       v8::Local<v8::String> key_str;
  //       if (!v8::String::NewFromUtf8(env->isolate, desc.utf8name, v8::NewStringType::kNormal)
  //                .ToLocal(&key_str)) {
  //         return napi_generic_failure;
  //       }
  //       key = key_str;
  //     } else if (desc.name != nullptr) {
  //       v8::Local<v8::Value> name_value = napi_v8_unwrap_value(desc.name);
  //       if (!name_value->IsName()) return napi_name_expected;
  //       key = name_value.As<v8::Name>();
  //     } else {
  //       return napi_name_expected;
  //     }
  //     v8::Local<v8::Object> target =
  //         (desc.attributes & napi_static) ? ctor.As<v8::Object>() : proto;
  //     if (desc.method != nullptr) {
  //       napi_value fnValue = nullptr;
  //       status = napi_create_function(
  //           env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, &fnValue);
  //       if (status != napi_ok) return status;
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_v8_unwrap_value(fnValue),
  //                ToV8PropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return napi_generic_failure;
  //       }
  //       continue;
  //     }
  //     if (desc.getter != nullptr || desc.setter != nullptr) {
  //       v8::Local<v8::Function> getter_fn;
  //       v8::Local<v8::Function> setter_fn;
  //       if (desc.getter != nullptr) {
  //         napi_value getter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, &getter_value);
  //         if (status != napi_ok) return status;
  //         getter_fn = napi_v8_unwrap_value(getter_value).As<v8::Function>();
  //       }
  //       if (desc.setter != nullptr) {
  //         napi_value setter_value = nullptr;
  //         status = napi_create_function(
  //             env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, &setter_value);
  //         if (status != napi_ok) return status;
  //         setter_fn = napi_v8_unwrap_value(setter_value).As<v8::Function>();
  //       }
  //       target->SetAccessorProperty(
  //           key,
  //           getter_fn,
  //           setter_fn,
  //           ToV8PropertyAttributes(desc.attributes, false));
  //       continue;
  //     }
  //     if (desc.value != nullptr) {
  //       if (!target->DefineOwnProperty(
  //                context,
  //                key,
  //                napi_v8_unwrap_value(desc.value),
  //                ToV8PropertyAttributes(desc.attributes, true))
  //                .FromMaybe(false)) {
  //         return napi_generic_failure;
  //       }
  //       continue;
  //     }
  //   }
  //   *result = ctorValue;
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_new_instance(napi_env env,
  //                                          napi_value constructor,
  //                                          size_t argc,
  //                                          const napi_value* argv,
  //                                          napi_value* result) {
  //   if (!CheckValue(env, constructor) || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> ctorValue = napi_v8_unwrap_value(constructor);
  //   if (!ctorValue->IsFunction()) return napi_function_expected;
  //   v8::Local<v8::Function> ctor = ctorValue.As<v8::Function>();
  //   std::vector<v8::Local<v8::Value>> args;
  //   args.reserve(argc);
  //   for (size_t i = 0; i < argc; ++i) args.push_back(napi_v8_unwrap_value(argv[i]));
  //   v8::Local<v8::Value> out;
  //   v8::TryCatch tryCatch(env->isolate);
  //   if (!ctor->NewInstance(env->context(), static_cast<int>(argc), args.data())
  //            .ToLocal(&out)) {
  //     if (tryCatch.HasCaught()) {
  //       SetLastException(env, tryCatch.Exception(), tryCatch.Message());
  //       return napi_v8_set_last_error(env, napi_pending_exception, "Constructor threw");
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  napi_status NAPI_CDECL napi_call_function(napi_env env,
                                            napi_value recv,
                                            napi_value func,
                                            size_t argc,
                                            const napi_value *argv,
                                            napi_value *result)
  {
    // 1. Basic Validation
    if (!CheckEnv(env) || func == nullptr)
    {
      return InvalidArg(env);
    }
    if (argc > 0 && argv == nullptr)
    {
      return InvalidArg(env);
    }

    JSValue js_func = napi_quickjs_unwrap_value(func);

    // 2. Ensure the target is actually a function
    if (!JS_IsFunction(env->ctx, js_func))
    {
      return napi_quickjs_set_last_error(env, napi_function_expected, "Target is not a function");
    }

    // 3. Resolve the receiver ('this' object)
    // If recv is null, Node-API defaults to 'undefined'
    JSValue js_recv = (recv != nullptr) ? napi_quickjs_unwrap_value(recv) : JS_UNDEFINED;

    // 4. Prepare the arguments array
    // We use a small stack buffer for performance, falling back to a heap vector for many args.
    JSValue *js_argv = nullptr;
    if (argc > 0)
    {
      // TODO: Use JSRuntime allocator, and ensure memory is freed afterwards!
      js_argv = static_cast<JSValue *>(alloca(sizeof(JSValue) * argc));
      for (size_t i = 0; i < argc; i++)
      {
        js_argv[i] = napi_quickjs_unwrap_value(argv[i]);
      }
    }

    // 5. Perform the call
    JSValue js_result = JS_Call(env->ctx, js_func, js_recv, (int)argc, js_argv);

    // 6. Handle Exceptions
    if (JS_IsException(js_result))
    {
      return ReturnPendingIfCaught(env, "Exception during function call");
    }

    // 7. Handle the Result
    if (result != nullptr)
    {
      // Wrap the result; napi_quickjs_wrap_value should handle JSValue ownership
      *result = napi_quickjs_wrap_value(env, js_result);
    }
    else
    {
      // If the caller doesn't want the result, we must free it to avoid leaks
      JS_FreeValue(env->ctx, js_result);
    }

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_define_properties(napi_env env,
                                                napi_value object,
                                                size_t property_count,
                                                const napi_property_descriptor *properties)
  {
    if (!CheckValue(env, object))
      return InvalidArg(env);

    if (property_count > 0 && properties == nullptr)
      return napi_invalid_arg;

    JSValue obj = napi_quickjs_unwrap_value(object);
    if (!JS_IsObject(obj))
      return napi_object_expected;

    for (size_t i = 0; i < property_count; i++)
    {
      const napi_property_descriptor &p = properties[i];
      JSAtom prop_name = JS_ATOM_NULL;

      // 1. Resolve the Property Name (Atom)
      if (p.utf8name != nullptr)
      {
        prop_name = JS_NewAtom(env->ctx, p.utf8name);
      }
      else if (p.name != nullptr)
      {
        JSValue name_val = napi_quickjs_unwrap_value(p.name);
        prop_name = JS_ValueToAtom(env->ctx, name_val);
      }
      else
      {
        return napi_invalid_arg;
      }

      if (prop_name == JS_ATOM_NULL)
        return ReturnPendingIfCaught(env, "Failed to create Atom");

      // 2. Map Node-API attributes to QuickJS flags
      int flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
      if (p.attributes & napi_enumerable)
        flags |= JS_PROP_ENUMERABLE;
      if (p.attributes & napi_configurable)
        flags |= JS_PROP_CONFIGURABLE;

      napi_status status = napi_ok;

      // 3. Define Accessors (Getter/Setter)
      if (p.getter != nullptr || p.setter != nullptr)
      {
        JSValue getter = JS_UNDEFINED;
        JSValue setter = JS_UNDEFINED;

        if (p.getter != nullptr)
        {
          getter = napi_quickjs_create_function_internal(env, p.utf8name, p.getter, p.data);
          if (JS_IsException(getter))
          {
            JS_FreeAtom(env->ctx, prop_name);
            return ReturnPendingIfCaught(env, "Failed to create getter");
          }
        }

        if (p.setter != nullptr)
        {
          setter = napi_quickjs_create_function_internal(env, p.utf8name, p.setter, p.data);
          if (JS_IsException(setter))
          {
            JS_FreeValue(env->ctx, getter);
            JS_FreeAtom(env->ctx, prop_name);
            return ReturnPendingIfCaught(env, "Failed to create setter");
          }
        }

        if (JS_DefineProperty(env->ctx, obj, prop_name, JS_UNDEFINED, getter, setter,
                              flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET) < 0)
        {
          status = ReturnPendingIfCaught(env, "Failed to define accessor");
        }

        JS_FreeValue(env->ctx, getter);
        JS_FreeValue(env->ctx, setter);
      }

      // 4. Define Methods
      else if (p.method != nullptr)
      {
        JSValue method_fn = napi_quickjs_create_function_internal(env, p.utf8name, p.method, p.data);
        if (JS_IsException(method_fn))
        {
          JS_FreeAtom(env->ctx, prop_name);
          return ReturnPendingIfCaught(env, "Failed to create method");
        }

        int method_flags = flags | JS_PROP_HAS_VALUE;
        if (p.attributes & napi_writable)
          method_flags |= JS_PROP_WRITABLE | JS_PROP_HAS_WRITABLE;

        if (JS_DefinePropertyValue(env->ctx, obj, prop_name, method_fn, method_flags) < 0)
        {
          status = ReturnPendingIfCaught(env, "Failed to define method");
        }
      }

      // 5. Define Data Properties (Value)
      else
      {
        int data_flags = flags | JS_PROP_HAS_VALUE;
        if (p.attributes & napi_writable)
          data_flags |= JS_PROP_WRITABLE | JS_PROP_HAS_WRITABLE;

        JSValue value = JS_DupValue(env->ctx, napi_quickjs_unwrap_value(p.value));
        if (JS_DefinePropertyValue(env->ctx, obj, prop_name, value, data_flags) < 0)
        {
          status = ReturnPendingIfCaught(env, "Failed to define data property");
        }
      }

      JS_FreeAtom(env->ctx, prop_name);
      if (status != napi_ok)
        return status;
    }

    return napi_ok;
  }

  // napi_status NAPI_CDECL napi_create_promise(napi_env env,
  //                                            napi_deferred* deferred,
  //                                            napi_value* promise) {
  //   if (!CheckEnv(env) || deferred == nullptr || promise == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch tc(env->isolate);
  //   v8::Local<v8::Promise::Resolver> resolver;
  //   if (!v8::Promise::Resolver::New(env->context()).ToLocal(&resolver)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to create Promise resolver");
  //   }
  //   auto* d = new (std::nothrow) napi_deferred__();
  //   if (d == nullptr) return napi_generic_failure;
  //   d->env = env;
  //   d->resolver.Reset(env->isolate, resolver);
  //   *deferred = d;
  //   *promise = napi_v8_wrap_value(env, resolver->GetPromise());
  //   if (*promise == nullptr) {
  //     delete d;
  //     *deferred = nullptr;
  //     return napi_generic_failure;
  //   }
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
  //                                              napi_deferred deferred,
  //                                              napi_value resolution) {
  //   if (!CheckEnv(env) || deferred == nullptr || resolution == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch tc(env->isolate);
  //   v8::Local<v8::Promise::Resolver> resolver = deferred->resolver.Get(env->isolate);
  //   if (!resolver->Resolve(env->context(), napi_v8_unwrap_value(resolution)).FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to resolve promise");
  //   }
  //   delete deferred;
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
  //                                             napi_deferred deferred,
  //                                             napi_value rejection) {
  //   if (!CheckEnv(env) || deferred == nullptr || rejection == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch tc(env->isolate);
  //   v8::Local<v8::Promise::Resolver> resolver = deferred->resolver.Get(env->isolate);
  //   if (!resolver->Reject(env->context(), napi_v8_unwrap_value(rejection)).FromMaybe(false)) {
  //     return ReturnPendingIfCaught(env, tc, "Failed to reject promise");
  //   }
  //   delete deferred;
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool* is_promise) {
  //   if (!CheckEnv(env) || value == nullptr || is_promise == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *is_promise = napi_v8_unwrap_value(value)->IsPromise();
  //   return napi_v8_clear_last_error(env);
  // }

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

  // napi_status NAPI_CDECL napi_get_property_names(napi_env env,
  //                                                napi_value object,
  //                                                napi_value* result) {
  //   return napi_v8_internal::GetPropertyNames(env,
  //                                             object,
  //                                             v8::KeyCollectionMode::kIncludePrototypes,
  //                                             napi_key_enumerable | napi_key_skip_symbols,
  //                                             v8::IndexFilter::kIncludeIndices,
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
  //   v8::KeyCollectionMode collection_mode =
  //       (key_mode == napi_key_own_only) ? v8::KeyCollectionMode::kOwnOnly
  //                                       : v8::KeyCollectionMode::kIncludePrototypes;
  //   return napi_v8_internal::GetPropertyNames(env,
  //                                             object,
  //                                             collection_mode,
  //                                             static_cast<uint32_t>(key_filter),
  //                                             v8::IndexFilter::kIncludeIndices,
  //                                             key_conversion,
  //                                             "Exception while getting all property names",
  //                                             result);
  // }

  napi_status NAPI_CDECL napi_set_named_property(napi_env env,
                                                 napi_value object,
                                                 const char *utf8name,
                                                 napi_value value)
  {
    // 1. Basic Validation
    if (!CheckEnv(env) || !CheckValue(env, object) || utf8name == nullptr || !CheckValue(env, value))
    {
      return InvalidArg(env);
    }

    JSValue obj = napi_quickjs_unwrap_value(object);

    // 2. Ensure the target is an object
    if (!JS_IsObject(obj))
    {
      return napi_quickjs_set_last_error(env, napi_object_expected, "Target is not an object");
    }

    // 3. Unwrap the value to be set
    JSValue val = napi_quickjs_unwrap_value(value);

    // 4. Set the property.
    // We use JS_DupValue(val) because JS_SetPropertyStr takes ownership of the value.
    if (JS_SetPropertyStr(env->ctx, obj, utf8name, JS_DupValue(env->ctx, val)) < 0)
    {
      return ReturnPendingIfCaught(env, "Failed to set named property");
    }

    return napi_quickjs_clear_last_error(env);
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

  // napi_status NAPI_CDECL napi_get_prototype(napi_env env,
  //                                           napi_value object,
  //                                           napi_value* result) {
  //   if (!CheckValue(env, object) || result == nullptr) return InvalidArg(env);
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   v8::Local<v8::Value> proto = target.As<v8::Object>()->GetPrototypeV2();
  //   *result = napi_v8_wrap_value(env, proto);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_set_prototype(napi_env env,
  //                                               napi_value object,
  //                                               napi_value value) {
  //   if (!CheckValue(env, object) || !CheckValue(env, value)) return napi_invalid_arg;
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<v8::Object>()
  //            ->SetPrototypeV2(env->context(), napi_v8_unwrap_value(value))
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

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

  // napi_status NAPI_CDECL napi_get_value_string_latin1(
  //     napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  //   if (!CheckEnv(env) || value == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsString()) {
  //     return napi_v8_set_last_error(env, napi_string_expected, "A string was expected");
  //   }
  //   v8::Local<v8::String> str = local.As<v8::String>();
  //   if (buf == nullptr) {
  //     if (result == nullptr) {
  //       return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     }
  //     *result = str->Length();
  //   } else if (bufsize != 0) {
  //     uint32_t length = static_cast<uint32_t>(
  //         std::min(bufsize - 1, static_cast<size_t>(str->Length())));
  //     str->WriteOneByteV2(env->isolate,
  //                         0,
  //                         length,
  //                         reinterpret_cast<uint8_t*>(buf),
  //                         v8::String::WriteFlags::kNullTerminate);
  //     if (result != nullptr) *result = length;
  //   } else if (result != nullptr) {
  //     *result = 0;
  //   }
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env,
  //                                                    napi_value value,
  //                                                    char16_t* buf,
  //                                                    size_t bufsize,
  //                                                    size_t* result) {
  //   if (!CheckEnv(env) || value == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsString()) {
  //     return napi_v8_set_last_error(env, napi_string_expected, "A string was expected");
  //   }
  //   v8::Local<v8::String> str = local.As<v8::String>();
  //   if (buf == nullptr) {
  //     if (result == nullptr) {
  //       return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //     }
  //     *result = str->Length();
  //   } else if (bufsize != 0) {
  //     uint32_t length = static_cast<uint32_t>(
  //         std::min(bufsize - 1, static_cast<size_t>(str->Length())));
  //     str->WriteV2(env->isolate,
  //                  0,
  //                  length,
  //                  reinterpret_cast<uint16_t*>(buf),
  //                  v8::String::WriteFlags::kNullTerminate);
  //     if (result != nullptr) *result = length;
  //   } else if (result != nullptr) {
  //     *result = 0;
  //   }
  //   return napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env,
  //                                            napi_value value,
  //                                            napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   *result = napi_v8_wrap_value(
  //       env, v8::Boolean::New(env->isolate, napi_v8_unwrap_value(value)->BooleanValue(env->isolate)));
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_number(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch try_catch(env->isolate);
  //   v8::Local<v8::Number> out;
  //   if (!napi_v8_unwrap_value(value)->ToNumber(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_v8_set_last_error(env, napi_pending_exception, "Exception during number coercion");
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_object(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch try_catch(env->isolate);
  //   v8::Local<v8::Object> out;
  //   if (!napi_v8_unwrap_value(value)->ToObject(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_v8_set_last_error(env, napi_pending_exception, "Exception during object coercion");
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_coerce_to_string(napi_env env,
  //                                              napi_value value,
  //                                              napi_value* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   v8::TryCatch try_catch(env->isolate);
  //   v8::Local<v8::String> out;
  //   if (!napi_v8_unwrap_value(value)->ToString(env->context()).ToLocal(&out)) {
  //     if (try_catch.HasCaught()) {
  //       SetLastException(env, try_catch.Exception(), try_catch.Message());
  //     }
  //     return napi_v8_set_last_error(env, napi_pending_exception, "Exception during string coercion");
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_v8_clear_last_error(env);
  // }

  // napi_status NAPI_CDECL napi_get_value_external(napi_env env,
  //                                                napi_value value,
  //                                                void** result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> local = napi_v8_unwrap_value(value);
  //   if (!local->IsExternal()) return napi_invalid_arg;
  //   *result = local.As<v8::External>()->Value();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_strict_equals(napi_env env,
  //                                           napi_value lhs,
  //                                           napi_value rhs,
  //                                           bool* result) {
  //   if (!CheckValue(env, lhs) || !CheckValue(env, rhs) || result == nullptr) {
  //     return napi_invalid_arg;
  //   }
  //   *result = napi_v8_unwrap_value(lhs)->StrictEquals(napi_v8_unwrap_value(rhs));
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_reference(napi_env env,
  //                                              napi_value value,
  //                                              uint32_t initial_refcount,
  //                                              napi_ref* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   *result = new (std::nothrow)
  //       napi_ref__(env, napi_v8_unwrap_value(value), initial_refcount);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env, napi_ref ref) {
  //   (void)env;
  //   if (ref == nullptr) return napi_invalid_arg;
  //   // If this weak reference is being deleted while a GC pass is active, V8 may
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
  //       ref->value.SetWeak(ref, ReferenceWeakCallback, v8::WeakCallbackType::kParameter);
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
  //   *result = napi_v8_wrap_value(env, ref->value.Get(env->isolate));
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_wrap(napi_env env,
  //                                  napi_value js_object,
  //                                  void* native_object,
  //                                  node_api_basic_finalize finalize_cb,
  //                                  void* finalize_hint,
  //                                  napi_ref* result) {
  //   if (!CheckValue(env, js_object)) return napi_invalid_arg;
  //   v8::Local<v8::Value> value = napi_v8_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;
  //   v8::Local<v8::Object> object = value.As<v8::Object>();
  //   v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  //   v8::Local<v8::Value> existing;
  //   if (object->GetPrivate(env->context(), wrapKey).ToLocal(&existing) &&
  //       existing->IsExternal()) {
  //     return napi_v8_set_last_error(env, napi_invalid_arg, "Invalid argument");
  //   }
  //   if (!object->SetPrivate(env->context(), wrapKey, v8::External::New(env->isolate, native_object))
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Private> wrapFinalizeKey = env->wrap_finalizer_private_key.Get(env->isolate);
  //   if (finalize_cb != nullptr) {
  //     auto* record = new (std::nothrow) WrapFinalizerRecord();
  //     if (record == nullptr) return napi_generic_failure;
  //     record->env = env;
  //     record->native_object = native_object;
  //     record->finalize_cb = finalize_cb;
  //     record->finalize_hint = finalize_hint;
  //     record->handle.Reset(env->isolate, object);
  //     record->handle.SetWeak(record, WrapWeakCallback, v8::WeakCallbackType::kParameter);
  //     env->wrap_finalizers.push_back(record);
  //     object
  //         ->SetPrivate(env->context(), wrapFinalizeKey, v8::External::New(env->isolate, record))
  //         .FromMaybe(false);
  //   } else {
  //     object->DeletePrivate(env->context(), wrapFinalizeKey).FromMaybe(false);
  //   }
  //   if (result != nullptr) {
  //     napi_status s = napi_create_reference(env, js_object, 0, result);
  //     if (s != napi_ok) return s;
  //     v8::Local<v8::Private> wrapRefKey = env->wrap_ref_private_key.Get(env->isolate);
  //     object
  //         ->SetPrivate(env->context(), wrapRefKey, v8::External::New(env->isolate, *result))
  //         .FromMaybe(false);
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void** result) {
  //   if (!CheckValue(env, js_object) || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> value = napi_v8_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;
  //   v8::Local<v8::Object> object = value.As<v8::Object>();
  //   v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  //   v8::Local<v8::Value> wrapped;
  //   if (!object->GetPrivate(env->context(), wrapKey).ToLocal(&wrapped) ||
  //       !wrapped->IsExternal()) {
  //     return napi_invalid_arg;
  //   }
  //   *result = wrapped.As<v8::External>()->Value();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
  //   if (!CheckValue(env, js_object)) return napi_invalid_arg;
  //   void* out = nullptr;
  //   napi_status status = napi_unwrap(env, js_object, &out);
  //   if (status != napi_ok) return status;
  //   v8::Local<v8::Object> object = napi_v8_unwrap_value(js_object).As<v8::Object>();
  //   v8::Local<v8::Private> wrapKey = env->wrap_private_key.Get(env->isolate);
  //   object->DeletePrivate(env->context(), wrapKey).FromMaybe(false);
  //   v8::Local<v8::Private> wrapRefKey = env->wrap_ref_private_key.Get(env->isolate);
  //   object->DeletePrivate(env->context(), wrapRefKey).FromMaybe(false);
  //   v8::Local<v8::Private> wrapFinalizeKey = env->wrap_finalizer_private_key.Get(env->isolate);
  //   v8::Local<v8::Value> finalizeValue;
  //   if (object->GetPrivate(env->context(), wrapFinalizeKey).ToLocal(&finalizeValue) &&
  //       finalizeValue->IsExternal()) {
  //     auto* record = static_cast<WrapFinalizerRecord*>(finalizeValue.As<v8::External>()->Value());
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

  napi_status NAPI_CDECL napi_throw_error(napi_env env,
                                          const char *code,
                                          const char *msg)
  {
    if (!CheckEnv(env))
    {
      return napi_invalid_arg;
    }

    // 1. Create the Error object
    JSValue error = JS_NewError(env->ctx);

    // 2. Set the message property
    JS_SetPropertyStr(env->ctx, error, "message",
                      JS_NewString(env->ctx, msg ? msg : ""));

    // 3. Set the code property if provided
    if (code != nullptr)
    {
      JS_SetPropertyStr(env->ctx, error, "code",
                        JS_NewString(env->ctx, code));
    }

    // 4. Transfer ownership of the error object to the engine's exception slot
    // JS_Throw returns the exception value, which we don't need to capture here.
    JS_Throw(env->ctx, error);

    return napi_ok;
  }

  // napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  //   if (!CheckValue(env, error)) return napi_invalid_arg;
  //   v8::Local<v8::Value> ex = napi_v8_unwrap_value(error);
  //   env->isolate->ThrowException(ex);
  //   SetLastException(env, ex);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
  //   if (!CheckValue(env, value) || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> v = napi_v8_unwrap_value(value);
  //   *result = v->IsNativeError();
  //   return napi_ok;
  // }

  // static napi_status CreateErrorCommon(napi_env env,
  //                                      v8::Local<v8::Value> (*factory)(v8::Local<v8::String>),
  //                                      napi_value code,
  //                                      napi_value msg,
  //                                      napi_value* result) {
  //   if (!CheckEnv(env) || msg == nullptr || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> msg_val = napi_v8_unwrap_value(msg);
  //   if (!msg_val->IsString()) return napi_string_expected;
  //   v8::Local<v8::String> message = msg_val.As<v8::String>();
  //   v8::Local<v8::Value> created = factory(message);
  //   if (!created->IsObject()) return napi_generic_failure;
  //   v8::Local<v8::Object> err_obj = created.As<v8::Object>();
  //   if (code != nullptr) {
  //     v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
  //     err_obj->Set(env->context(), code_key, napi_v8_unwrap_value(code)).FromMaybe(false);
  //   }
  //   *result = napi_v8_wrap_value(env, err_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_create_error(napi_env env,
  //                                          napi_value code,
  //                                          napi_value msg,
  //                                          napi_value* result) {
  //   return CreateErrorCommon(
  //       env,
  //       [](v8::Local<v8::String> message) { return v8::Exception::Error(message); },
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
  //       [](v8::Local<v8::String> message) { return v8::Exception::TypeError(message); },
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
  //       [](v8::Local<v8::String> message) { return v8::Exception::RangeError(message); },
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
  //       [](v8::Local<v8::String> message) { return v8::Exception::SyntaxError(message); },
  //       code,
  //       msg,
  //       result);
  // }

  // napi_status NAPI_CDECL napi_throw_type_error(napi_env env,
  //                                              const char* code,
  //                                              const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   v8::Local<v8::String> message;
  //   if (!v8::String::NewFromUtf8(env->isolate,
  //                                (msg == nullptr) ? "Type error" : msg,
  //                                v8::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Object> err = v8::Exception::TypeError(message).As<v8::Object>();
  //   if (code != nullptr) {
  //     v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
  //     v8::Local<v8::String> code_val;
  //     if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->isolate->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL napi_throw_range_error(napi_env env,
  //                                               const char* code,
  //                                               const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   v8::Local<v8::String> message;
  //   if (!v8::String::NewFromUtf8(env->isolate,
  //                                (msg == nullptr) ? "Range error" : msg,
  //                                v8::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Object> err = v8::Exception::RangeError(message).As<v8::Object>();
  //   if (code != nullptr) {
  //     v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
  //     v8::Local<v8::String> code_val;
  //     if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->isolate->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  // napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env,
  //                                                    const char* code,
  //                                                    const char* msg) {
  //   if (!CheckEnv(env)) return napi_invalid_arg;
  //   v8::Local<v8::String> message;
  //   if (!v8::String::NewFromUtf8(env->isolate,
  //                                (msg == nullptr) ? "Syntax error" : msg,
  //                                v8::NewStringType::kNormal)
  //            .ToLocal(&message)) {
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Object> err = v8::Exception::SyntaxError(message).As<v8::Object>();
  //   if (code != nullptr) {
  //     v8::Local<v8::String> code_key = v8::String::NewFromUtf8Literal(env->isolate, "code");
  //     v8::Local<v8::String> code_val;
  //     if (v8::String::NewFromUtf8(env->isolate, code, v8::NewStringType::kNormal).ToLocal(&code_val)) {
  //       err->Set(env->context(), code_key, code_val).FromMaybe(false);
  //     }
  //   }
  //   env->isolate->ThrowException(err);
  //   SetLastException(env, err);
  //   return napi_pending_exception;
  // }

  napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool *result)
  {
    if (!CheckEnv(env) || result == nullptr)
    {
      return napi_invalid_arg;
    }

    // Check both the engine state and our internal environment cache
    *result = JS_HasException(env->ctx) || !JS_IsUndefined(env->last_exception);

    return napi_ok;
  }

  // napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env,
  //                                                          napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if (env->last_exception.IsEmpty()) return napi_generic_failure;
  //   v8::Local<v8::Value> ex = env->last_exception.Get(env->isolate);
  //   napi_value wrapped = napi_v8_wrap_value(env, ex);
  //   if (wrapped != nullptr &&
  //       env->last_exception_source_line.empty() &&
  //       env->last_exception_thrown_at.empty()) {
  //     v8::Local<v8::Message> message;
  //     if (!env->last_exception_message.IsEmpty()) {
  //       message = env->last_exception_message.Get(env->isolate);
  //     }
  //     if (!message.IsEmpty()) {
  //       const std::string source_line =
  //           unofficial_napi_internal::GetErrorSourceLineForStderrImpl(env, message);
  //       unofficial_napi_internal::SetArrowMessageFromString(
  //           env->isolate, env->context(), ex, source_line);
  //       unofficial_napi_internal::PreserveErrorFormatting(
  //           env,
  //           ex,
  //           source_line,
  //           unofficial_napi_internal::GetThrownAtString(env->isolate, message));
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
  //   *result = napi_v8_wrap_value(env, ex);
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
  //   v8::Local<v8::Value> source = napi_v8_unwrap_value(script);
  //   if (!source->IsString()) return napi_string_expected;
  //   v8::TryCatch tc(env->isolate);
  //   v8::Local<v8::Script> compiled;
  //   if (!v8::Script::Compile(env->context(), source.As<v8::String>()).ToLocal(&compiled)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_pending_exception;
  //     }
  //     return napi_generic_failure;
  //   }
  //   v8::Local<v8::Value> out;
  //   if (!compiled->Run(env->context()).ToLocal(&out)) {
  //     if (tc.HasCaught()) {
  //       SetLastException(env, tc.Exception(), tc.Message());
  //       return napi_pending_exception;
  //     }
  //     return napi_generic_failure;
  //   }
  //   *result = napi_v8_wrap_value(env, out);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  //   if (!CheckEnv(env) || err == nullptr) return napi_invalid_arg;
  //   SetLastException(env, napi_v8_unwrap_value(err));
  //   env->isolate->ThrowException(napi_v8_unwrap_value(err));
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
  //   v8::Local<v8::Context> context = env->context();
  //   v8::Context::Scope context_scope(context);
  //   auto backing = v8::ArrayBuffer::NewBackingStore(env->isolate, length);
  //   if (!backing) return napi_generic_failure;
  //   *data = backing->Data();
  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = std::move(backing);
  //   v8::Local<v8::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  //   record->holder.Reset(env->isolate, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  //   v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);
  //   *result = napi_v8_wrap_value(env, buffer_obj);
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
  //   v8::Local<v8::Context> context = env->context();
  //   v8::Context::Scope context_scope(context);
  //   auto* hint = new (std::nothrow) napi_external_backing_store_hint__();
  //   if (hint == nullptr) return napi_generic_failure;
  //   hint->env = env;
  //   hint->external_data = data;
  //   hint->finalize_cb = finalize_cb;
  //   hint->finalize_hint = finalize_hint;
  //   std::unique_ptr<v8::BackingStore> backing =
  //       v8::ArrayBuffer::NewBackingStore(data, length, ExternalBackingStoreDeleter, hint);
  //   if (!backing) {
  //     delete hint;
  //     return napi_generic_failure;
  //   }
  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = std::move(backing);
  //   v8::Local<v8::Object> buffer_obj = CreateBufferObject(env, record->backing_store, 0, length);
  //   record->holder.Reset(env->isolate, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  //   v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);
  //   *result = napi_v8_wrap_value(env, buffer_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  //   if (!CheckEnv(env) || value == nullptr || result == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  //   *result = raw->IsArrayBufferView();
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
  //                                             napi_value value,
  //                                             void** data,
  //                                             size_t* length) {
  //   if (!CheckEnv(env) || value == nullptr) return napi_invalid_arg;
  //   if (!GetArrayBufferViewInfo(napi_v8_unwrap_value(value), data, length)) {
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
  //   v8::Local<v8::Value> raw = napi_v8_unwrap_value(arraybuffer);
  //   if (!raw->IsArrayBuffer()) return napi_invalid_arg;
  //   v8::Local<v8::ArrayBuffer> ab = raw.As<v8::ArrayBuffer>();
  //   size_t ab_length = ab->ByteLength();
  //   if (byte_offset > ab_length || byte_length > (ab_length - byte_offset)) {
  //     return napi_invalid_arg;
  //   }
  //   auto* record = new (std::nothrow) napi_buffer_record__();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->backing_store = ab->GetBackingStore();
  //   v8::Local<v8::Object> buffer_obj =
  //       CreateBufferObject(env, record->backing_store, byte_offset, byte_length);
  //   record->holder.Reset(env->isolate, buffer_obj);
  //   record->holder.SetWeak(record, BufferWeakCallback, v8::WeakCallbackType::kParameter);
  //   v8::Local<v8::Private> key = env->buffer_private_key.Get(env->isolate);
  //   buffer_obj
  //       ->SetPrivate(env->context(), key, v8::External::New(env->isolate, record))
  //       .FromJust();
  //   env->buffer_records.push_back(record);
  //   *result = napi_v8_wrap_value(env, buffer_obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

  // napi_status NAPI_CDECL napi_adjust_external_memory(
  //     node_api_basic_env basic_env, int64_t change_in_bytes, int64_t* adjusted_value) {
  //   napi_env env = const_cast<napi_env>(basic_env);
  //   if (!CheckEnv(env) || adjusted_value == nullptr) return napi_invalid_arg;
  //   *adjusted_value = env->isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_add_finalizer(napi_env env,
  //                                           napi_value js_object,
  //                                           void* finalize_data,
  //                                           node_api_basic_finalize finalize_cb,
  //                                           void* finalize_hint,
  //                                           napi_ref* result) {
  //   if (!CheckValue(env, js_object) || finalize_cb == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> value = napi_v8_unwrap_value(js_object);
  //   if (!value->IsObject()) return napi_object_expected;
  //   auto* record = new (std::nothrow) WrapFinalizerRecord();
  //   if (record == nullptr) return napi_generic_failure;
  //   record->env = env;
  //   record->native_object = finalize_data;
  //   record->finalize_cb = finalize_cb;
  //   record->finalize_hint = finalize_hint;
  //   record->handle.Reset(env->isolate, value.As<v8::Object>());
  //   record->handle.SetWeak(record, WrapWeakCallback, v8::WeakCallbackType::kParameter);
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
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<v8::Object>()
  //            ->SetIntegrityLevel(env->context(), v8::IntegrityLevel::kFrozen)
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  //   if (!CheckValue(env, object)) return napi_invalid_arg;
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(object);
  //   if (!target->IsObject()) return napi_object_expected;
  //   if (!target.As<v8::Object>()
  //            ->SetIntegrityLevel(env->context(), v8::IntegrityLevel::kSealed)
  //            .FromMaybe(false)) {
  //     return napi_generic_failure;
  //   }
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL napi_type_tag_object(
  //     napi_env env, napi_value value, const napi_type_tag* type_tag) {
  //   if (!CheckValue(env, value) || type_tag == nullptr) return napi_invalid_arg;
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(value);
  //   if (!target->IsObject() && !target->IsExternal()) return napi_invalid_arg;
  //   for (auto& entry : env->type_tag_entries) {
  //     if (!entry.value.IsEmpty() && entry.value.Get(env->isolate)->StrictEquals(target)) {
  //       entry.tag = *type_tag;
  //       return napi_ok;
  //     }
  //   }
  //   napi_env__::TypeTagEntry entry;
  //   entry.value.Reset(env->isolate, target);
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
  //   v8::Local<v8::Value> target = napi_v8_unwrap_value(value);
  //   if (!target->IsObject() && !target->IsExternal()) {
  //     *result = false;
  //     return napi_ok;
  //   }
  //   for (auto& entry : env->type_tag_entries) {
  //     if (entry.value.IsEmpty()) continue;
  //     if (entry.value.Get(env->isolate)->StrictEquals(target)) {
  //       *result = (entry.tag.lower == type_tag->lower && entry.tag.upper == type_tag->upper);
  //       return napi_ok;
  //     }
  //   }
  //   *result = false;
  //   return napi_ok;
  // }

  // napi_status NAPI_CDECL node_api_create_object_with_properties(napi_env env,
  //                                        napi_value prototype_or_null,
  //                                        napi_value* property_names,
  //                                        napi_value* property_values,
  //                                        size_t property_count,
  //                                        napi_value* result) {
  //   if (!CheckEnv(env) || result == nullptr) return napi_invalid_arg;
  //   if ((property_count > 0) && (property_names == nullptr || property_values == nullptr)) {
  //     return napi_invalid_arg;
  //   }
  //   v8::Local<v8::Object> obj = v8::Object::New(env->isolate);
  //   if (prototype_or_null != nullptr) {
  //     v8::Local<v8::Value> proto = napi_v8_unwrap_value(prototype_or_null);
  //     if (!proto->IsNull() && !proto->IsObject()) return napi_object_expected;
  //     if (!obj->SetPrototypeV2(env->context(), proto).FromMaybe(false)) {
  //       return napi_generic_failure;
  //     }
  //   }
  //   for (size_t i = 0; i < property_count; ++i) {
  //     if (property_names[i] == nullptr || property_values[i] == nullptr) return napi_invalid_arg;
  //     if (!obj
  //              ->Set(env->context(),
  //                    napi_v8_unwrap_value(property_names[i]),
  //                    napi_v8_unwrap_value(property_values[i]))
  //              .FromMaybe(false)) {
  //       return napi_generic_failure;
  //     }
  //   }
  //   *result = napi_v8_wrap_value(env, obj);
  //   return (*result == nullptr) ? napi_generic_failure : napi_ok;
  // }

} // extern "C"