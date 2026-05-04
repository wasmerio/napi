#include "internal/quickjs_env.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

enum
{
  __JS_ATOM_NULL = JS_ATOM_NULL,
#define DEF(name, str) JS_ATOM_##name,
#include "quickjs-atom.h"
#undef DEF
  JS_ATOM_END,
};

struct napi_external_backing_store_hint__
{
  napi_env env = nullptr;
  void *external_data = nullptr;
  node_api_basic_finalize finalize_cb = nullptr;
  void *finalize_hint = nullptr;
  JSValue weak_target = JS_UNDEFINED;
};
using napi_external_backing_store_hint = napi_external_backing_store_hint__;

static JSClassID napi_external_class_id = 0;

static bool IsRefEmptyValue(JSValue value)
{
  return JS_IsUninitialized(value) || JS_IsUndefined(value) || JS_IsNull(value);
}

static bool SameRefCountedValue(JSValueConst left, JSValueConst right)
{
  return JS_VALUE_HAS_REF_COUNT(left) &&
         JS_VALUE_HAS_REF_COUNT(right) &&
         JS_VALUE_GET_TAG(left) == JS_VALUE_GET_TAG(right) &&
         JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
}

static void RemoveWeakRef(napi_env env, napi_ref ref)
{
  if (env == nullptr || ref == nullptr)
    return;

  auto &refs = env->weak_refs;
  for (auto it = refs.begin(); it != refs.end(); ++it)
  {
    if (*it == ref)
    {
      refs.erase(it);
      return;
    }
  }
}

static void TrackWeakRef(napi_env env, napi_ref ref)
{
  if (env == nullptr || ref == nullptr || !ref->can_be_weak || ref->ref_count != 0)
    return;

  for (auto *tracked : env->weak_refs)
  {
    if (tracked == ref)
      return;
  }
  env->weak_refs.push_back(ref);
}

static void ClearWeakRefsForValue(napi_env env, JSValueConst value)
{
  if (env == nullptr || !JS_VALUE_HAS_REF_COUNT(value))
    return;

  for (auto *ref : env->weak_refs)
  {
    if (ref != nullptr && ref->ref_count == 0 &&
        !IsRefEmptyValue(ref->value) &&
        SameRefCountedValue(ref->value, value))
    {
      ref->value = JS_UNDEFINED;
    }
  }
}

// A fixed property name unlikely to collide with user properties.
// Leading/trailing underscores + napi prefix makes it effectively private.
static const char kTypeTagProperty[] = "__napi_type_tag__";
static const char kWrapProperty[] = "__napi_wrap__";
static const char kBufferProperty[] = "__napi_buffer__";
static const char kFinalizerProperty[] = "__napi_finalizer__";

static void napi_quickjs_external_finalizer(JSRuntime *rt, JSValue val)
{
  // Extract the hint struct we attached to the object
  auto *hint = static_cast<napi_external_backing_store_hint *>(JS_GetOpaque(val, napi_external_class_id));
  if (hint != nullptr)
  {
    JSValue target = JS_IsUndefined(hint->weak_target) ? val : hint->weak_target;
    ClearWeakRefsForValue(hint->env, target);

    // Call the Node-API finalizer callback if the user provided one
    if (hint->finalize_cb != nullptr)
    {
      hint->finalize_cb(hint->env, hint->external_data, hint->finalize_hint);
    }
    // Delete the hint struct itself
    delete hint;
  }
}

int RegisterExternalClass(JSRuntime *rt)
{
  JS_NewClassID(rt, &napi_external_class_id);
  JSClassDef def = {};
  def.class_name = "NapiExternal";
  def.finalizer = napi_quickjs_external_finalizer;
  return JS_NewClass(rt, napi_external_class_id, &def);
}

void *GetExternalValue(JSValue local)
{
  // Get the opaque data, ensuring the object is actually of our external class
  auto hint = static_cast<napi_external_backing_store_hint *>(
      JS_GetOpaque(local, napi_external_class_id));

  if (hint == nullptr)
  {
    return nullptr;
  }

  // Return the original raw C pointer
  return hint->external_data;
}

namespace
{
  napi_external_backing_store_hint__ *GetWrapRecord(JSContext *ctx, JSValue obj)
  {
    auto *wrap = static_cast<napi_external_backing_store_hint__ *>(
        JS_GetOpaque(obj, napi_external_class_id));
    if (wrap != nullptr)
      return wrap;

    JSValue stored = JS_GetPropertyStr(ctx, obj, kWrapProperty);
    if (JS_IsException(stored) || JS_IsUndefined(stored))
    {
      if (JS_IsException(stored))
      {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
      }
      return nullptr;
    }

    wrap = static_cast<napi_external_backing_store_hint__ *>(
        JS_GetOpaque(stored, napi_external_class_id));
    JS_FreeValue(ctx, stored);
    return wrap;
  }

  inline bool CheckEnv(napi_env env)
  {
    return env != nullptr && env->ctx != nullptr;
  }

  inline bool CheckValue(napi_env env, napi_value value)
  {
    return CheckEnv(env) && value != nullptr;
  }

  inline bool IsEmptyRef(napi_ref r)
  {
    return JS_IsUninitialized(r->value) || JS_IsUndefined(r->value) || JS_IsNull(r->value);
  }

  inline bool CanBeHeldWeakly(JSValue value)
  {
    return (JS_VALUE_HAS_REF_COUNT(value));
  }

  void ClearLastException(napi_env env)
  {
    if (env == nullptr)
      return;

    if (!env->has_last_exception)
      return;

    JS_FreeValue(env->ctx, env->last_exception);

    env->last_exception = JS_UNDEFINED;
    env->has_last_exception = false;
  }

  void SetLastException(napi_env env, JSValue exception)
  {
    if (env == nullptr)
      return;

    ClearLastException(env);

    env->last_exception = exception;
    env->has_last_exception = true;
  }

  inline bool RethrowLastException(napi_env env, JSContext *ctx)
  {
    if (!env->has_last_exception)
      return false;

    auto exception = env->last_exception;
    env->last_exception = JS_UNDEFINED;
    env->has_last_exception = false;

    JS_Throw(ctx, exception);
    return true;
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

  bool DecimalDigitsFit(const char *value, const char *max)
  {
    while (*value == '0' && value[1] != '\0')
      ++value;
    size_t value_len = std::strlen(value);
    size_t max_len = std::strlen(max);
    if (value_len != max_len)
      return value_len < max_len;
    return std::strcmp(value, max) <= 0;
  }

  bool BigIntFitsSigned64(JSContext *ctx, JSValueConst value)
  {
    const char *str = JS_ToCString(ctx, value);
    if (str == nullptr)
      return false;
    bool negative = str[0] == '-';
    bool fits = DecimalDigitsFit(negative ? str + 1 : str,
                                 negative ? "9223372036854775808" : "9223372036854775807");
    JS_FreeCString(ctx, str);
    return fits;
  }

  bool BigIntFitsUnsigned64(JSContext *ctx, JSValueConst value)
  {
    const char *str = JS_ToCString(ctx, value);
    if (str == nullptr)
      return false;
    bool fits = str[0] != '-' && DecimalDigitsFit(str, "18446744073709551615");
    JS_FreeCString(ctx, str);
    return fits;
  }

  std::vector<uint64_t> BigIntWordsFromDecimal(JSContext *ctx, JSValueConst value, bool *negative)
  {
    std::vector<uint64_t> words;
    const char *str = JS_ToCString(ctx, value);
    if (str == nullptr)
      return words;

    const char *cursor = str;
    *negative = cursor[0] == '-';
    if (*negative)
      ++cursor;

    for (; *cursor != '\0'; ++cursor)
    {
      if (*cursor < '0' || *cursor > '9')
        continue;

      unsigned carry = static_cast<unsigned>(*cursor - '0');
      for (size_t i = 0; i < words.size(); ++i)
      {
        unsigned __int128 next = static_cast<unsigned __int128>(words[i]) * 10 + carry;
        words[i] = static_cast<uint64_t>(next);
        carry = static_cast<unsigned>(next >> 64);
      }
      if (carry != 0 || words.empty())
        words.push_back(carry);
    }

    while (words.size() > 1 && words.back() == 0)
      words.pop_back();

    JS_FreeCString(ctx, str);
    return words;
  }

  std::vector<char> Utf8ToLatin1(const char *str, size_t len)
  {
    std::vector<char> out;
    for (size_t i = 0; i < len;)
    {
      unsigned char c = static_cast<unsigned char>(str[i]);
      uint32_t cp = c;
      size_t advance = 1;
      if ((c & 0xe0) == 0xc0 && i + 1 < len)
      {
        cp = ((c & 0x1f) << 6) | (static_cast<unsigned char>(str[i + 1]) & 0x3f);
        advance = 2;
      }
      else if ((c & 0xf0) == 0xe0 && i + 2 < len)
      {
        cp = ((c & 0x0f) << 12) |
             ((static_cast<unsigned char>(str[i + 1]) & 0x3f) << 6) |
             (static_cast<unsigned char>(str[i + 2]) & 0x3f);
        advance = 3;
      }
      else if ((c & 0xf8) == 0xf0 && i + 3 < len)
      {
        cp = '?';
        advance = 4;
      }
      out.push_back(static_cast<char>(cp <= 0xff ? cp : '?'));
      i += advance;
    }
    return out;
  }

  size_t CompleteUtf8PrefixLength(const char *str, size_t len)
  {
    size_t i = 0;
    while (i < len)
    {
      unsigned char c = static_cast<unsigned char>(str[i]);
      size_t width = 1;
      if ((c & 0x80) == 0)
        width = 1;
      else if ((c & 0xe0) == 0xc0)
        width = 2;
      else if ((c & 0xf0) == 0xe0)
        width = 3;
      else if ((c & 0xf8) == 0xf0)
        width = 4;
      else
        break;
      if (i + width > len)
        break;
      i += width;
    }
    return i;
  }

  napi_status MarkBuffer(napi_env env, JSValue value)
  {
    if (JS_DefinePropertyValueStr(env->ctx, value, kBufferProperty,
                                  JS_NewBool(env->ctx, true),
                                  JS_PROP_CONFIGURABLE) < 0)
      return ReturnPendingIfCaught(env, "Failed to mark Buffer");
    return napi_ok;
  }

  bool IsBufferValue(napi_env env, JSValue value)
  {
    if (!JS_IsObject(value))
      return false;
    JSValue marker = JS_GetPropertyStr(env->ctx, value, kBufferProperty);
    if (JS_IsException(marker))
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      return false;
    }
    bool is_buffer = JS_ToBool(env->ctx, marker) == 1;
    JS_FreeValue(env->ctx, marker);
    return is_buffer;
  }

  napi_status GetBufferInfo(napi_env env, JSValue value, void **data, size_t *length)
  {
    if (!IsBufferValue(env, value))
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    size_t offset = 0;
    size_t byte_len = 0;
    JSValue arraybuffer = JS_GetTypedArrayBuffer(env->ctx, value, &offset, &byte_len, nullptr);
    if (JS_IsException(arraybuffer))
      return ReturnPendingIfCaught(env, "Failed to get Buffer backing store");

    size_t arraybuffer_len = 0;
    uint8_t *arraybuffer_data = JS_GetArrayBuffer(env->ctx, &arraybuffer_len, arraybuffer);
    JS_FreeValue(env->ctx, arraybuffer);
    if (arraybuffer_data == nullptr && JS_HasException(env->ctx))
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }

    if (data != nullptr)
      *data = arraybuffer_data == nullptr ? nullptr : arraybuffer_data + offset;
    if (length != nullptr)
      *length = byte_len;
    return napi_quickjs_clear_last_error(env);
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

  inline bool FromQuickJSArrayType(int type, napi_typedarray_type *out)
  {
    switch (type)
    {
    case JS_TYPED_ARRAY_INT8:
      *out = napi_int8_array;
      return true;
    case JS_TYPED_ARRAY_UINT8:
      *out = napi_uint8_array;
      return true;
    case JS_TYPED_ARRAY_UINT8C:
      *out = napi_uint8_clamped_array;
      return true;
    case JS_TYPED_ARRAY_INT16:
      *out = napi_int16_array;
      return true;
    case JS_TYPED_ARRAY_UINT16:
      *out = napi_uint16_array;
      return true;
    case JS_TYPED_ARRAY_INT32:
      *out = napi_int32_array;
      return true;
    case JS_TYPED_ARRAY_UINT32:
      *out = napi_uint32_array;
      return true;
    case JS_TYPED_ARRAY_FLOAT32:
      *out = napi_float32_array;
      return true;
    case JS_TYPED_ARRAY_FLOAT64:
      *out = napi_float64_array;
      return true;
    case JS_TYPED_ARRAY_BIG_INT64:
      *out = napi_bigint64_array;
      return true;
    case JS_TYPED_ARRAY_BIG_UINT64:
      *out = napi_biguint64_array;
      return true;
    case JS_TYPED_ARRAY_FLOAT16:
      *out = napi_float16_array;
      return true;
    default:
      return false;
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
} // namespace

napi_ref__::napi_ref__(napi_env env, JSValue local, uint32_t initial_ref_count)
    : env(env), value(local), ref_count(initial_ref_count), can_be_weak(CanBeHeldWeakly(local))
{
  if (ref_count > 0)
  {
    // Promote to strong reference
    JS_DupValue(env->ctx, local);
  }
}

napi_ref__::~napi_ref__() = default;

napi_env__::napi_env__(JSContext *context, int32_t module_api_version)
    : ctx{context},
      last_exception{JS_UNDEFINED},
      module_api_version(module_api_version)
{
  root_scope = napi_scope__::create(this, nullptr);
  current_scope = root_scope;

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
  for (auto it = env_cleanup_hooks.rbegin(); it != env_cleanup_hooks.rend(); ++it)
  {
    auto *entry = *it;
    if (entry != nullptr && entry->hook != nullptr)
      entry->hook(entry->arg);
    delete entry;
  }
  env_cleanup_hooks.clear();

  // TODO: Need to implement cleanup
  // RunEnvCleanupHooks(this);
  // napi_v8_finalize_buffer_records(this);

  // for (auto* raw_record : wrap_finalizers) {
  //   auto* record = static_cast<wrapFinalizerRecord*>(raw_record);
  //   if (record != nullptr) {
  //     InvokewrapFinalizer(record);
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

  root_scope = nullptr;
  current_scope = nullptr;
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

static JSValue napi_quickjs_create_function_internal(napi_env env,
                                                     const char *utf8name,
                                                     napi_callback cb,
                                                     void *data)
{
  napi_value fn_val;
  napi_status status = napi_create_function(env, utf8name, NAPI_AUTO_LENGTH, cb, data, &fn_val);
  if (status != napi_ok)
    return JS_EXCEPTION;

  return JS_DupValue(env->ctx, fn_val->get_inner());
}

static JSValue napi_quickjs_trampoline(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv,
                                       int magic, JSValue *func_data)
{
  auto env = static_cast<napi_env>(JS_GetContextOpaque(ctx));
  if (!CheckEnv(env))
    return JS_ThrowReferenceError(ctx, "Null NAPI env");

  auto cb_ptr = GetExternalValue(func_data[0]);
  if (cb_ptr == nullptr)
    return JS_ThrowReferenceError(ctx, "Null NAPI callback data");

  auto cb = reinterpret_cast<napi_callback>(cb_ptr);
  auto user_data = GetExternalValue(func_data[1]);

  JSValue effective_this = this_val;
  JSValue new_target = JS_UNDEFINED;
  bool called_as_constructor =
      magic == JS_CFUNC_constructor_magic ||
      (magic == 0 && JS_IsConstructor(ctx, this_val));

  if (called_as_constructor)
  {
    // Allocate a fresh instance using the class that supports SetOpaque.
    // For JS_CLASS_C_FUNCTION_DATA constructors QuickJS passes new.target
    // as this_val, so the instance prototype follows new.target.prototype.
    JSValue proto = JS_GetProperty(ctx, this_val, JS_ATOM_prototype);
    effective_this = JS_NewObjectProtoClass(ctx, proto, napi_external_class_id);
    JS_FreeValue(ctx, proto);

    if (JS_IsException(effective_this))
      return effective_this;

    new_target = this_val;
  }

  auto info = napi_callback_info__{env, effective_this, new_target, argc, argv, user_data};
  auto result = cb(env, reinterpret_cast<napi_callback_info>(&info));

  if (RethrowLastException(env, ctx))
  {
    if (called_as_constructor)
      JS_FreeValue(ctx, effective_this);
    return JS_EXCEPTION;
  }

  JSValue returned = JS_UNDEFINED;
  if (called_as_constructor)
  {
    // Node-API constructors return _this, but in QuickJS constructor
    // magic mode the return value IS the new instance. If the callback
    // returned something explicit (e.g. a different object), use that,
    // otherwise return our allocated effective_this.
    if (result != nullptr)
    {
      JS_FreeValue(ctx, effective_this);
      returned = JS_DupValue(ctx, result->get_inner());
      env->current_scope->delete_value(result);
    }
    else
    {
      returned = effective_this; // most common path
    }
  }
  else if (result != nullptr)
  {
    returned = JS_DupValue(ctx, result->get_inner());
    env->current_scope->delete_value(result);
  }

  return returned;
}

static napi_status MakeFunctionConstructible(napi_env env, JSValue fn)
{
  JSValue proto = JS_NewObject(env->ctx);
  if (JS_IsException(proto))
    return ReturnPendingIfCaught(env, "Failed to create function prototype");

  JS_SetConstructor(env->ctx, fn, proto);
  JS_SetConstructorBit(env->ctx, fn, true);
  JS_FreeValue(env->ctx, proto);
  return napi_ok;
}

napi_status CreateFunction(napi_env env,
                           const char *utf8name,
                           size_t length,
                           napi_callback cb,
                           void *data,
                           int magic,
                           napi_value *result)
{
  if (!CheckEnv(env) || cb == nullptr || result == nullptr)
  {
    return InvalidArg(env);
  }

  // 1. wrap the callback and user data into externals so we can
  // pass them safely as "data" to the QuickJS function.
  napi_value cb_external, data_external;
  napi_create_external(env, reinterpret_cast<void *>(cb), nullptr, nullptr, &cb_external);
  napi_create_external(env, data, nullptr, nullptr, &data_external);

  JSValue data_values[2];
  data_values[0] = JS_DupValue(env->ctx, cb_external->get_inner());
  data_values[1] = JS_DupValue(env->ctx, data_external->get_inner());

  // 2. Create the C function with data
  // JS_NewCFunctionData allows us to attach 'magic' values to the function object.
  // NOTE: data_values are js_dup() -ed, need to free them
  JSValue fn = JS_NewCFunctionData(env->ctx, napi_quickjs_trampoline,
                                   0, magic, 2, data_values);
  JS_FreeValue(env->ctx, data_values[0]);
  JS_FreeValue(env->ctx, data_values[1]);

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

  *result = env->current_scope->wrap_value(fn, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

// Translate napi_key_filter + collection mode into JS_GPN_* flags
int NapiKeyFilterToGPN(napi_key_filter key_filter)
{
  int flags = 0;

  // Include string-keyed properties unless skip_strings is set
  if (!(key_filter & napi_key_skip_strings))
    flags |= JS_GPN_STRING_MASK;

  // Include symbol-keyed properties unless skip_symbols is set
  if (!(key_filter & napi_key_skip_symbols))
    flags |= JS_GPN_SYMBOL_MASK;

  // Enumerable-only filter
  if (key_filter & napi_key_enumerable)
    flags |= JS_GPN_ENUM_ONLY;

  return flags;
}

napi_status GetPropertyNames(napi_env env,
                             napi_value object,
                             napi_key_collection_mode key_mode,
                             napi_key_filter key_filter,
                             napi_key_conversion key_conversion,
                             napi_value *result)
{
  if (!CheckValue(env, object) || result == nullptr)
    return InvalidArg(env);

  JSContext *ctx = env->ctx;
  JSValue obj = object->get_inner();
  if (!JS_IsObject(obj))
    return napi_object_expected;

  int gpn_flags = NapiKeyFilterToGPN(key_filter);
  JSPropertyEnum *tab = nullptr;
  uint32_t tab_count = 0;

  // Collect own properties first
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_count, obj, gpn_flags) < 0)
    return ReturnPendingIfCaught(env, "Exception while getting property names");

  JSValue arr = JS_NewArray(ctx);
  uint32_t arr_idx = 0;

  auto passes_descriptor_filter = [&](JSValue owner, JSAtom atom) -> int
  {
    if (!(key_filter & (napi_key_writable | napi_key_configurable)))
      return 1;

    JSPropertyDescriptor desc;
    int has = JS_GetOwnProperty(ctx, &desc, owner, atom);
    if (has <= 0)
      return has;

    bool include = true;
    if ((key_filter & napi_key_writable) && !(desc.flags & JS_PROP_WRITABLE))
      include = false;
    if ((key_filter & napi_key_configurable) && !(desc.flags & JS_PROP_CONFIGURABLE))
      include = false;

    JS_FreeValue(ctx, desc.value);
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return include ? 1 : 0;
  };

  auto append_tab = [&](JSValue owner, JSPropertyEnum *t, uint32_t count) -> napi_status
  {
    for (uint32_t i = 0; i < count; ++i)
    {
      int include = passes_descriptor_filter(owner, t[i].atom);
      if (include < 0)
      {
        JS_FreePropertyEnum(ctx, t, count);
        return ReturnPendingIfCaught(env, "Exception while filtering property names");
      }
      if (include == 0)
        continue;

      JSValue key;
      if (key_conversion == napi_key_numbers_to_strings)
      {
        key = JS_AtomToValue(ctx, t[i].atom);
        if (!JS_IsSymbol(key))
        {
          JS_FreeValue(ctx, key);
          key = JS_AtomToString(ctx, t[i].atom);
        }
      }
      else
      {
        // napi_key_keep_numbers: integer atoms stay as numbers, others as strings/symbols
        key = JS_AtomToValue(ctx, t[i].atom);
      }
      if (JS_IsException(key))
      {
        JS_FreePropertyEnum(ctx, t, count);
        return ReturnPendingIfCaught(env, "Failed to convert property name");
      }
      JS_SetPropertyUint32(ctx, arr, arr_idx++, key);
    }
    JS_FreePropertyEnum(ctx, t, count);
    return napi_ok;
  };

  napi_status status = append_tab(obj, tab, tab_count);
  if (status != napi_ok)
  {
    JS_FreeValue(ctx, arr);
    return status;
  }

  // Walk the prototype chain for kIncludePrototypes
  if (key_mode == napi_key_include_prototypes)
  {
    JSValue proto = JS_GetPrototype(ctx, obj);
    while (JS_IsObject(proto))
    {
      JSPropertyEnum *ptab = nullptr;
      uint32_t pcount = 0;
      if (JS_GetOwnPropertyNames(ctx, &ptab, &pcount, proto, gpn_flags) == 0)
      {
        status = append_tab(proto, ptab, pcount);
        if (status != napi_ok)
        {
          JS_FreeValue(ctx, proto);
          JS_FreeValue(ctx, arr);
          return status;
        }
      }

      JSValue next = JS_GetPrototype(ctx, proto);
      JS_FreeValue(ctx, proto);
      proto = next;
    }
    JS_FreeValue(ctx, proto);
  }

  *result = env->current_scope->wrap_value(arr, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

JSValue CreatePlainError(JSContext *ctx, const char *msg)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");
  JSValue msg_str = JS_NewString(ctx, msg ? msg : "");
  JSValue error = JS_CallConstructor(ctx, error_ctor, 1, &msg_str);
  JS_FreeValue(ctx, msg_str);
  JS_FreeValue(ctx, error_ctor);
  JS_FreeValue(ctx, global);
  return error;
}

static napi_status CreatePlainErrorCommon(napi_env env, napi_value code, napi_value msg, napi_value *result)
{
  if (!CheckEnv(env) || msg == nullptr || result == nullptr)
    return napi_invalid_arg;

  JSValue msg_val = msg->get_inner();
  if (!JS_IsString(msg_val))
    return napi_string_expected;

  const char *msg_str = JS_ToCString(env->ctx, msg_val);
  JSValue error = CreatePlainError(env->ctx, msg_str);
  JS_FreeCString(env->ctx, msg_str);

  if (code != nullptr)
  {
    const char *code_str = JS_ToCString(env->ctx, code->get_inner());
    JS_SetPropertyStr(env->ctx, error, "code", JS_NewString(env->ctx, code_str));
    JS_FreeCString(env->ctx, code_str);
  }

  *result = env->current_scope->wrap_value(error, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

JSValue CreateErrorObject(JSContext *ctx, JSValue (*factory)(JSContext *, const char *, ...), const char *code, const char *msg)
{
  JSValue error = factory(ctx, "%s", msg ? msg : "");
  JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, msg ? msg : ""));
  if (code != nullptr)
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
  return error;
}

napi_status CreateErrorCommon(napi_env env, JSValue (*factory)(JSContext *, const char *, ...),
                              napi_value code, napi_value msg, napi_value *result)
{
  if (!CheckEnv(env) || msg == nullptr || result == nullptr)
    return napi_invalid_arg;

  JSValue msg_val = msg->get_inner();
  if (!JS_IsString(msg_val))
    return napi_string_expected;

  const char *msg_str = JS_ToCString(env->ctx, msg_val);
  const char *code_str = nullptr;
  if (code != nullptr)
    code_str = JS_ToCString(env->ctx, code->get_inner());

  JSValue error = CreateErrorObject(env->ctx, factory, code_str, msg_str);

  JS_FreeCString(env->ctx, msg_str);
  if (code_str != nullptr)
    JS_FreeCString(env->ctx, code_str);

  *result = env->current_scope->wrap_value(error, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
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
    *result = env->current_scope->wrap_value(JS_UNDEFINED, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NULL, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    auto context = env->context();
    *result = env->current_scope->wrap_value(JS_GetGlobalObject(context), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_boolean(napi_env env,
                                          bool value,
                                          napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewBool(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_double(napi_env env,
                                            double value,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewFloat64(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_int32(napi_env env,
                                           int32_t value,
                                           napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewInt32(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_int64(napi_env env,
                                           int64_t value,
                                           napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewInt64(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_uint32(napi_env env,
                                            uint32_t value,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewUint32(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env,
                                                  int64_t value,
                                                  napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewBigInt64(env->ctx, value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env,
                                                   uint64_t value,
                                                   napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewBigUint64(env->ctx, value), true);
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

    if (word_count > 1024)
    {
      JS_ThrowRangeError(env->ctx, "Maximum BigInt size exceeded");
      return ReturnPendingIfCaught(env, "Maximum BigInt size exceeded");
    }

    size_t used_words = word_count;
    while (used_words > 0 && words[used_words - 1] == 0)
      --used_words;

    if (used_words == 0)
    {
      *result = env->current_scope->wrap_value(JS_NewBigInt64(env->ctx, 0), true);
      return (*result == nullptr) ? napi_generic_failure : napi_ok;
    }

    std::string literal;
    if (sign_bit != 0)
      literal.push_back('-');
    literal += "0x";

    char chunk[17];
    std::snprintf(chunk, sizeof(chunk), "%llx",
                  static_cast<unsigned long long>(words[used_words - 1]));
    literal += chunk;

    for (size_t i = used_words - 1; i-- > 0;)
    {
      std::snprintf(chunk, sizeof(chunk), "%016llx",
                    static_cast<unsigned long long>(words[i]));
      literal += chunk;
    }

    literal.push_back('n');
    JSValue bigint = JS_Eval(env->ctx, literal.c_str(), literal.size(),
                             "<napi_create_bigint_words>", JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(bigint))
      return ReturnPendingIfCaught(env, "Failed to create BigInt from words");

    *result = env->current_scope->wrap_value(bigint, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
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
    *result = env->current_scope->wrap_value(out, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewObject(env->ctx), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    *result = env->current_scope->wrap_value(JS_NewArray(env->ctx), true);
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

    // 4. wrap it in a napi_value and return
    *result = env->current_scope->wrap_value(obj, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_external(napi_env env,
                                                 napi_value value,
                                                 void **result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    *result = GetExternalValue(value->get_inner());

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
                                false);
    if (JS_IsException(ab))
    {
      js_free_rt(rt, buf);
      return ReturnPendingIfCaught(env, "Failed to create array buffer");
    }

    *result = env->current_scope->wrap_value(ab, true);
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
                              false);

      if (JS_IsException(out))
      {
        delete hint;
        return ReturnPendingIfCaught(env, "Failed to create external array");
      }
    }

    *result = env->current_scope->wrap_value(out, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool *result)
  {
    if (!CheckEnv(env) || value == nullptr || result == nullptr)
      return napi_invalid_arg;
    *result = JS_GetTypedArrayType(value->get_inner()) >= 0;
    return napi_quickjs_clear_last_error(env);
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
        arraybuffer->get_inner(),
        JS_NewInt64(env->ctx, static_cast<int64_t>(byte_offset)),
        JS_NewInt64(env->ctx, static_cast<int64_t>(length))};

    JSTypedArrayEnum array_type = ToQuickJSArrayType(type);

    JSValue view = JS_NewTypedArray(env->ctx, 3, argv, array_type);
    JS_FreeValue(env->ctx, argv[1]);
    JS_FreeValue(env->ctx, argv[2]);

    if (JS_IsException(view))
    {
      return ReturnPendingIfCaught(env, "Failed to create TypedArray");
    }

    *result = env->current_scope->wrap_value(view, true);
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

    JSValue local = typedarray->get_inner();
    int type_idx = JS_GetTypedArrayType(local);
    if (type_idx < 0)
      return napi_invalid_arg;

    if (type != nullptr)
    {
      if (!FromQuickJSArrayType(type_idx, type))
        return napi_invalid_arg;
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
      case JS_TYPED_ARRAY_INT8:
      case JS_TYPED_ARRAY_UINT8:
      case JS_TYPED_ARRAY_UINT8C:
        shift = 0;
        break;
      case JS_TYPED_ARRAY_INT16:
      case JS_TYPED_ARRAY_UINT16:
      case JS_TYPED_ARRAY_FLOAT16:
        shift = 1;
        break;
      case JS_TYPED_ARRAY_INT32:
      case JS_TYPED_ARRAY_UINT32:
      case JS_TYPED_ARRAY_FLOAT32:
        shift = 2;
        break;
      case JS_TYPED_ARRAY_FLOAT64:
      case JS_TYPED_ARRAY_BIG_INT64:
      case JS_TYPED_ARRAY_BIG_UINT64:
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
      *arraybuffer = env->current_scope->wrap_value(abuf, true);
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
    JSValue local = arraybuffer->get_inner();

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
    JSValue local = value->get_inner();

    if (JS_IsUndefined(local) || JS_IsNull(local))
    {
      *result = true;
      return napi_ok;
    }

    if (!JS_IsArrayBuffer(local))
    {
      if (JS_IsObject(local))
      {
        JSValue byte_len = JS_GetPropertyStr(env->ctx, local, "byteLength");
        if (JS_IsException(byte_len))
        {
          JSValue exc = JS_GetException(env->ctx);
          JS_FreeValue(env->ctx, exc);
        }
        else if (!JS_IsUndefined(byte_len))
        {
          JS_FreeValue(env->ctx, byte_len);
          *result = true;
          return napi_ok;
        }
        else
        {
          JS_FreeValue(env->ctx, byte_len);
        }
      }
      return napi_arraybuffer_expected;
    }

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
      // ES: Expextation is that length is capped at numerix limit of uint32_t
      int64_t length_i64 = static_cast<int64_t>(
          length > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
              ? std::numeric_limits<uint32_t>::max()
              : length);

      if (JS_SetLength(env->ctx, arr, length_i64) < 0)
      {
        JS_FreeValue(env->ctx, arr);
        return ReturnPendingIfCaught(env, "Failed to set array length");
      }
    }

    *result = env->current_scope->wrap_value(arr, true);
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
    if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    JSValue out = JS_NewStringLen(env->ctx, str, length);
    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Cannot create string");
    }

    *result = env->current_scope->wrap_value(out, true);
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
    if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

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

    *result = env->current_scope->wrap_value(out, true);
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
    if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

    JSValue out = JS_NewStringUTF16(env->ctx, reinterpret_cast<const uint16_t *>(str), length);

    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Cannot create string");
    }

    *result = env->current_scope->wrap_value(out, true);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL node_api_create_external_string_latin1(
      napi_env env, char *str, size_t length,
      node_api_basic_finalize finalize_callback, void *finalize_hint,
      napi_value *result, bool *copied)
  {
    // QuickJS always copies string data, so finalize_callback/finalize_hint
    // are intentionally ignored — the caller retains ownership of str.
    (void)finalize_callback;
    (void)finalize_hint;
    if (copied != nullptr)
      *copied = false;
    napi_status status = napi_create_string_latin1(env, str, length, result);
    if (status == napi_ok && finalize_callback != nullptr)
      finalize_callback(env, str, finalize_hint);
    return status;
  }

  napi_status NAPI_CDECL node_api_create_external_string_utf16(
      napi_env env, char16_t *str, size_t length,
      node_api_basic_finalize finalize_callback, void *finalize_hint,
      napi_value *result, bool *copied)
  {
    (void)finalize_callback;
    (void)finalize_hint;
    if (copied != nullptr)
      *copied = false;
    napi_status status = napi_create_string_utf16(env, str, length, result);
    if (status == napi_ok && finalize_callback != nullptr)
      finalize_callback(env, str, finalize_hint);
    return status;
  }

  // Property keys in QuickJS are interned atoms. We create a regular string
  // which QuickJS will intern automatically when used as a property key.
  napi_status NAPI_CDECL node_api_create_property_key_latin1(
      napi_env env, const char *str, size_t length, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if (str == nullptr)
    {
      if (length != 0)
        return napi_invalid_arg;
      str = "";
    }
    if (length == NAPI_AUTO_LENGTH)
      length = strlen(str);

    return napi_create_string_latin1(env, str, length, result);
  }

  napi_status NAPI_CDECL node_api_create_property_key_utf8(
      napi_env env, const char *str, size_t length, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if (str == nullptr)
    {
      if (length != 0)
        return napi_invalid_arg;
      str = "";
    }
    if (length == NAPI_AUTO_LENGTH)
      length = strlen(str);

    *result = env->current_scope->wrap_value(JS_NewStringLen(env->ctx, str, length), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL node_api_create_property_key_utf16(
      napi_env env, const char16_t *str, size_t length, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if (str == nullptr)
    {
      if (length != 0)
        return napi_invalid_arg;
      str = u"";
    }
    if (length == NAPI_AUTO_LENGTH)
    {
      const char16_t *p = str;
      while (*p != 0)
        ++p;
      length = static_cast<size_t>(p - str);
    }

    *result = env->current_scope->wrap_value(
        JS_NewStringUTF16(env->ctx, reinterpret_cast<const uint16_t *>(str), length), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_symbol(napi_env env,
                                            napi_value description,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    const char *desc_str = nullptr;
    if (description != nullptr)
    {
      JSValue desc_val = description->get_inner();
      if (!JS_IsString(desc_val))
        return napi_string_expected;
      desc_str = JS_ToCString(env->ctx, desc_val);
    }

    JSValue sym = JS_NewSymbol(env->ctx, desc_str ? desc_str : "", false);

    if (desc_str != nullptr)
      JS_FreeCString(env->ctx, desc_str);

    *result = env->current_scope->wrap_value(sym, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL node_api_symbol_for(napi_env env,
                                             const char *utf8description,
                                             size_t length,
                                             napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if (utf8description == nullptr && length > 0)
      return napi_invalid_arg;

    const char *desc = (utf8description == nullptr) ? "" : utf8description;

    // JS_NewSymbol with is_global=true mirrors Symbol.for() — same key returns same symbol.
    JSValue sym = JS_NewSymbol(env->ctx, desc, true);

    *result = env->current_scope->wrap_value(sym, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_typeof(napi_env env,
                                     napi_value value,
                                     napi_valuetype *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    JSValue local = value->get_inner();

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
    JSValue local = value->get_inner();
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
    JSValue local = value->get_inner();
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
    JSValue local = value->get_inner();
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
    JSValue local = value->get_inner();
    if (!JS_IsNumber(local))
    {
      return napi_quickjs_set_last_error(env, napi_number_expected, "A number was expected");
    }

    double number = 0;
    if (JS_ToFloat64(env->ctx, &number, local) < 0)
    {
      return ReturnPendingIfCaught(env, "Exception during int64 coercion");
    }

    if (!std::isfinite(number))
    {
      *result = 0;
    }
    else if (number >= 9223372036854775808.0)
    {
      *result = std::numeric_limits<int64_t>::max();
    }
    else if (number <= -9223372036854775808.0)
    {
      *result = std::numeric_limits<int64_t>::min();
    }
    else
    {
      *result = static_cast<int64_t>(number);
    }

    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env,
                                                     napi_value value,
                                                     int64_t *result,
                                                     bool *lossless)
  {
    if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr)
      return napi_invalid_arg;

    JSValue local = value->get_inner();
    if (!JS_IsBigInt(local))
      return napi_bigint_expected;

    int rc = JS_ToBigInt64(env->ctx, result, local);
    *lossless = (rc == 0) && BigIntFitsSigned64(env->ctx, local);
    if (rc != 0)
    {
      // Clear the exception — lossless=false is the signal, not an error
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      // Saturate: re-extract as uint64 and reinterpret for the truncated bits
      uint64_t u = 0;
      JS_ToBigUint64(env->ctx, &u, local);
      *result = static_cast<int64_t>(u);
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env,
                                                      napi_value value,
                                                      uint64_t *result,
                                                      bool *lossless)
  {
    if (!CheckEnv(env) || value == nullptr || result == nullptr || lossless == nullptr)
      return napi_invalid_arg;

    JSValue local = value->get_inner();
    if (!JS_IsBigInt(local))
      return napi_bigint_expected;

    int rc = JS_ToBigUint64(env->ctx, result, local);
    *lossless = (rc == 0) && BigIntFitsUnsigned64(env->ctx, local);
    if (rc != 0)
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      // Truncated bits via int64 reinterpret
      int64_t s = 0;
      JS_ToBigInt64(env->ctx, &s, local);
      *result = static_cast<uint64_t>(s);
    }
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env,
                                                     napi_value value,
                                                     int *sign_bit,
                                                     size_t *word_count,
                                                     uint64_t *words)
  {
    if (!CheckEnv(env) || value == nullptr || word_count == nullptr)
      return napi_invalid_arg;

    JSValue local = value->get_inner();
    if (!JS_IsBigInt(local))
      return napi_bigint_expected;

    bool negative = false;
    std::vector<uint64_t> bigint_words = BigIntWordsFromDecimal(env->ctx, local, &negative);
    if (bigint_words.empty())
      bigint_words.push_back(0);

    if (sign_bit != nullptr)
      *sign_bit = negative ? 1 : 0;

    if (words == nullptr)
    {
      *word_count = bigint_words.size();
      return napi_ok;
    }

    size_t capacity = *word_count;
    size_t copied = (capacity < bigint_words.size()) ? capacity : bigint_words.size();
    for (size_t i = 0; i < copied; ++i)
    {
      words[i] = bigint_words[i];
    }
    *word_count = bigint_words.size();
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool *is_date)
  {
    if (!CheckValue(env, value) || is_date == nullptr)
      return napi_invalid_arg;

    *is_date = JS_IsDate(value->get_inner());
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value, double *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    JSValue local = value->get_inner();
    if (!JS_IsDate(local))
      return napi_date_expected;

    // No JS_GetDateValue in QuickJS — call .valueOf() which returns the epoch ms
    JSValue valueOf = JS_GetPropertyStr(env->ctx, local, "valueOf");
    if (JS_IsException(valueOf))
      return ReturnPendingIfCaught(env, "Failed to get Date.valueOf");

    JSValue ms = JS_Call(env->ctx, valueOf, local, 0, nullptr);
    JS_FreeValue(env->ctx, valueOf);

    if (JS_IsException(ms))
      return ReturnPendingIfCaught(env, "Failed to call Date.valueOf");

    int rc = JS_ToFloat64(env->ctx, result, ms);
    JS_FreeValue(env->ctx, ms);

    if (rc != 0)
      return ReturnPendingIfCaught(env, "Failed to convert Date value to float64");

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);
    JSValue local = value->get_inner();
    if (JS_IsUndefined(local) || JS_IsNull(local))
    {
      *result = true;
      return napi_quickjs_clear_last_error(env);
    }
    *result = JS_IsArrayBuffer(local);
    if (!*result && JS_IsObject(local))
    {
      size_t len = 0;
      uint8_t *ptr = JS_GetArrayBuffer(env->ctx, &len, local);
      if (JS_HasException(env->ctx))
      {
        JSValue exc = JS_GetException(env->ctx);
        JS_FreeValue(env->ctx, exc);
      }
      else
      {
        *result = ptr != nullptr || len > 0;
      }
      if (!*result)
      {
        JSValue byte_len = JS_GetPropertyStr(env->ctx, local, "byteLength");
        if (JS_IsException(byte_len))
        {
          JSValue exc = JS_GetException(env->ctx);
          JS_FreeValue(env->ctx, exc);
        }
        else
        {
          *result = !JS_IsUndefined(byte_len);
          JS_FreeValue(env->ctx, byte_len);
        }
      }
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env,
                                                   napi_value arraybuffer,
                                                   void **data,
                                                   size_t *byte_length)
  {
    if (!CheckValue(env, arraybuffer))
      return InvalidArg(env);

    size_t len = 0;
    uint8_t *ptr = JS_GetArrayBuffer(env->ctx, &len, arraybuffer->get_inner());
    if (ptr == nullptr && len == 0 && JS_HasException(env->ctx))
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }

    if (data != nullptr)
      *data = ptr;
    if (byte_length != nullptr)
      *byte_length = len;
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL node_api_is_sharedarraybuffer(napi_env env,
                                                       napi_value value,
                                                       bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);

    if (JS_IsArrayBuffer(value->get_inner()))
    {
      *result = false;
      return napi_quickjs_clear_last_error(env);
    }

    size_t len = 0;
    uint8_t *ptr = JS_GetArrayBuffer(env->ctx, &len, value->get_inner());
    if (ptr == nullptr && len == 0 && JS_HasException(env->ctx))
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      *result = false;
      return napi_quickjs_clear_last_error(env);
    }
    *result = true;
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env,
                                                           size_t byte_length,
                                                           void **data,
                                                           napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return InvalidArg(env);

    auto rt = JS_GetRuntime(env->ctx);
    auto buf = static_cast<uint8_t *>(js_malloc_rt(rt, byte_length));
    if (byte_length > 0 && buf == nullptr)
      return napi_generic_failure;

    JSValue sab = JS_NewArrayBuffer(env->ctx, buf, byte_length, &FreeArrayBufferData, nullptr, true);
    if (JS_IsException(sab))
    {
      js_free_rt(rt, buf);
      return ReturnPendingIfCaught(env, "Failed to create SharedArrayBuffer");
    }

    if (data != nullptr)
      *data = buf;
    *result = env->current_scope->wrap_value(sab, true);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_create_dataview(napi_env env,
                                              size_t length,
                                              napi_value arraybuffer,
                                              size_t byte_offset,
                                              napi_value *result)
  {
    if (!CheckValue(env, arraybuffer) || result == nullptr)
      return InvalidArg(env);

    size_t ab_len = 0;
    uint8_t *ab_data = JS_GetArrayBuffer(env->ctx, &ab_len, arraybuffer->get_inner());
    if (ab_data == nullptr && ab_len == 0 && JS_HasException(env->ctx))
      return ReturnPendingIfCaught(env, "ArrayBuffer expected");

    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue ctor = JS_GetPropertyStr(env->ctx, global, "DataView");
    JS_FreeValue(env->ctx, global);
    if (JS_IsException(ctor))
      return ReturnPendingIfCaught(env, "Failed to get DataView constructor");

    JSValue args[] = {
        arraybuffer->get_inner(),
        JS_NewInt64(env->ctx, static_cast<int64_t>(byte_offset)),
        JS_NewInt64(env->ctx, static_cast<int64_t>(length))};
    JSValue view = JS_CallConstructor(env->ctx, ctor, 3, args);
    JS_FreeValue(env->ctx, args[1]);
    JS_FreeValue(env->ctx, args[2]);
    JS_FreeValue(env->ctx, ctor);
    if (JS_IsException(view))
      return ReturnPendingIfCaught(env, "DataView construction threw");

    *result = env->current_scope->wrap_value(view, true);
    return (*result == nullptr) ? napi_generic_failure : napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);
    *result = JS_IsDataView(value->get_inner());
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_dataview_info(napi_env env,
                                                napi_value dataview,
                                                size_t *byte_length,
                                                void **data,
                                                napi_value *arraybuffer,
                                                size_t *byte_offset)
  {
    if (!CheckValue(env, dataview))
      return InvalidArg(env);
    JSValue view = dataview->get_inner();
    if (!JS_IsDataView(view))
      return InvalidArg(env);

    JSValue len_val = JS_GetPropertyStr(env->ctx, view, "byteLength");
    JSValue offset_val = JS_GetPropertyStr(env->ctx, view, "byteOffset");
    JSValue buffer_val = JS_GetPropertyStr(env->ctx, view, "buffer");
    if (JS_IsException(len_val) || JS_IsException(offset_val) || JS_IsException(buffer_val))
    {
      JS_FreeValue(env->ctx, len_val);
      JS_FreeValue(env->ctx, offset_val);
      JS_FreeValue(env->ctx, buffer_val);
      return ReturnPendingIfCaught(env, "Failed to get DataView info");
    }

    uint32_t len = 0;
    uint32_t offset = 0;
    JS_ToUint32(env->ctx, &len, len_val);
    JS_ToUint32(env->ctx, &offset, offset_val);
    JS_FreeValue(env->ctx, len_val);
    JS_FreeValue(env->ctx, offset_val);

    if (byte_length != nullptr)
      *byte_length = len;
    if (byte_offset != nullptr)
      *byte_offset = offset;
    if (data != nullptr)
    {
      size_t ab_len = 0;
      uint8_t *ab_data = JS_GetArrayBuffer(env->ctx, &ab_len, buffer_val);
      if (ab_data == nullptr && ab_len == 0 && JS_HasException(env->ctx))
      {
        JS_FreeValue(env->ctx, buffer_val);
        return ReturnPendingIfCaught(env, "Failed to get DataView ArrayBuffer");
      }
      *data = ab_data + offset;
    }
    if (arraybuffer != nullptr)
    {
      *arraybuffer = env->current_scope->wrap_value(buffer_val, true);
      if (*arraybuffer == nullptr)
      {
        JS_FreeValue(env->ctx, buffer_val);
        return napi_generic_failure;
      }
    }
    else
    {
      JS_FreeValue(env->ctx, buffer_val);
    }
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;
    *result = JS_IsArray(value->get_inner());
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_array_length(napi_env env,
                                               napi_value value,
                                               uint32_t *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;
    JSValue local = value->get_inner();
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue out = JS_GetPropertyUint32(env->ctx, local, index);
    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Exception while getting element");
    }
    *result = env->current_scope->wrap_value(out, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_set_element(napi_env env,
                                          napi_value object,
                                          uint32_t index,
                                          napi_value value)
  {
    if (!CheckValue(env, object) || value == nullptr)
      return InvalidArg(env);
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    // JS_SetPropertyUint32 consumes the value, so we must duplicate it
    if (JS_SetPropertyUint32(env->ctx, local, index, JS_DupValue(env->ctx, value->get_inner())) < 0)
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

    JSValue val = object->get_inner();
    JSValue ctor = constructor->get_inner();

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
    JSValue local = object->get_inner();
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
    JSValue local = object->get_inner();
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
          argv[i] = env->current_scope->wrap_value(info->argv[i], false);
        }

        // Node-API Rule: If the user provided a larger buffer than actual arguments,
        // fill the remaining slots with 'undefined'.
        for (; i < *argc; i++)
        {
          argv[i] = env->current_scope->wrap_value(JS_UNDEFINED, true);
        }
      }

      // Always update *argc to the actual number of arguments available.
      *argc = (size_t)info->argc;
    }

    // 2. Handle the 'this' argument
    if (this_arg != nullptr)
    {
      *this_arg = env->current_scope->wrap_value(info->this_val, false);
    }

    // 3. Handle the user data pointer
    if (data != nullptr)
    {
      *data = info->data;
    }

    return napi_ok;
  }

  napi_status napi_get_new_target(napi_env env,
                                  napi_callback_info cbinfo,
                                  napi_value *result)
  {
    if (env == nullptr || cbinfo == nullptr || result == nullptr)
    {
      return napi_invalid_arg;
    }

    // In Node-API, if the function was NOT called with 'new',
    // new_target should return NULL (nullptr).
    if (JS_IsUndefined(cbinfo->new_target))
    {
      *result = nullptr;
    }
    else
    {
      *result = env->current_scope->wrap_value(cbinfo->new_target, false);
    }

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    void *memory = js_mallocz(env->ctx, sizeof(napi_handle_scope__));
    if (memory == nullptr)
      return napi_generic_failure;

    auto *scope = new (memory) napi_handle_scope__(env, env->current_scope);
    if (scope == nullptr)
      return napi_generic_failure;

    env->current_scope = scope;
    *result = scope;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope)
  {
    if (!CheckEnv(env) || scope == nullptr)
      return napi_invalid_arg;
    if (env->current_scope != scope)
      return InvalidArg(env);

    env->current_scope = scope->parent();
    scope->~napi_handle_scope__();
    js_free(env->ctx, scope);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_open_escapable_handle_scope(napi_env env,
                                                          napi_escapable_handle_scope *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    void *memory = js_mallocz(env->ctx, sizeof(napi_escapable_handle_scope__));
    if (memory == nullptr)
      return napi_generic_failure;

    auto *scope = new (memory) napi_escapable_handle_scope__(env, env->current_scope);
    if (scope == nullptr)
      return napi_generic_failure;

    env->current_scope = scope;
    *result = scope;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_close_escapable_handle_scope(napi_env env,
                                                           napi_escapable_handle_scope scope)
  {
    if (!CheckEnv(env) || scope == nullptr)
      return napi_invalid_arg;
    if (env->current_scope != scope)
      return InvalidArg(env);

    env->current_scope = scope->parent();
    scope->~napi_escapable_handle_scope__();
    js_free(env->ctx, scope);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_escape_handle(napi_env env,
                                            napi_escapable_handle_scope scope,
                                            napi_value escapee,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || scope == nullptr || escapee == nullptr || result == nullptr)
      return napi_invalid_arg;

    if (scope->escaped)
      return napi_escape_called_twice;

    scope->escaped = true;
    *result = scope->escape_value(escapee);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_function(napi_env env,
                                              const char *utf8name,
                                              size_t length,
                                              napi_callback cb,
                                              void *data,
                                              napi_value *result)
  {
    return CreateFunction(env, utf8name, length, cb, data, 0, result);
  }

  napi_status NAPI_CDECL napi_define_class(napi_env env,
                                           const char *utf8name,
                                           size_t length,
                                           napi_callback constructor,
                                           void *data,
                                           size_t property_count,
                                           const napi_property_descriptor *properties,
                                           napi_value *result)
  {
    if (env == nullptr)
      return napi_invalid_arg;
    if (utf8name == nullptr || constructor == nullptr || result == nullptr)
    {
      return InvalidArg(env);
    }
    if (property_count > 0 && properties == nullptr)
    {
      return InvalidArg(env);
    }

    JSContext *ctx = env->context();

    // 1. Create the constructor function
    // We reuse napi_create_function which handles the JS-to-C trampoline for Node-API.
    napi_value ctor_napi_value = nullptr;
    napi_status status = CreateFunction(
        env, utf8name, length, constructor, data, JS_CFUNC_constructor_magic, &ctor_napi_value);
    if (status != napi_ok)
      return status;

    // 2. Setup the prototype chain
    JSValue ctor = ctor_napi_value->get_inner();
    JSValue proto = JS_NewObject(ctx);

    JS_SetConstructor(ctx, ctor, proto);
    JS_SetConstructorBit(ctx, ctor, true);

    // 3. Iterate over the descriptors and define them
    for (size_t i = 0; i < property_count; ++i)
    {
      const napi_property_descriptor &desc = properties[i];

      // Target is Constructor for static methods, Prototype for instance methods
      JSValue target = (desc.attributes & napi_static) ? ctor : proto;

      // Resolve property key into a JSAtom
      JSAtom key;
      if (desc.utf8name != nullptr)
      {
        key = JS_NewAtom(ctx, desc.utf8name);
      }
      else if (desc.name != nullptr)
      {
        JSValue name_val = desc.name->get_inner();
        if (!JS_IsString(name_val) && !JS_IsSymbol(name_val))
        {
          JS_FreeValue(ctx, proto);
          return napi_name_expected;
        }
        key = JS_ValueToAtom(ctx, name_val);
      }
      else
      {
        JS_FreeValue(ctx, proto);
        return napi_name_expected;
      }

      // Map Node-API attributes to QuickJS internal property flags
      int base_flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
      if (desc.attributes & napi_enumerable)
        base_flags |= JS_PROP_ENUMERABLE;
      if (desc.attributes & napi_configurable)
        base_flags |= JS_PROP_CONFIGURABLE;

      if (desc.method != nullptr)
      {
        // --- Methods ---
        int method_flags = base_flags | JS_PROP_HAS_WRITABLE;
        if (desc.attributes & napi_writable)
          method_flags |= JS_PROP_WRITABLE;

        napi_value method_val = nullptr;
        status = CreateFunction(
            env, desc.utf8name, NAPI_AUTO_LENGTH, desc.method, desc.data, JS_CFUNC_generic_magic, &method_val);

        if (status == napi_ok)
        {
          JS_DefinePropertyValue(ctx, target, key,
                                 JS_DupValue(ctx, method_val->get_inner()),
                                 method_flags);
        }
      }
      else if (desc.getter != nullptr || desc.setter != nullptr)
      {
        // --- Accessors (Getters / Setters) ---
        JSValue getter = JS_UNDEFINED;
        JSValue setter = JS_UNDEFINED;

        if (desc.getter != nullptr)
        {
          napi_value g_val = nullptr;
          if (CreateFunction(env, desc.utf8name, NAPI_AUTO_LENGTH, desc.getter, desc.data, JS_CFUNC_getter_magic, &g_val) == napi_ok)
          {
            getter = JS_DupValue(ctx, g_val->get_inner());
          }
        }
        if (desc.setter != nullptr)
        {
          napi_value s_val = nullptr;
          if (CreateFunction(env, desc.utf8name, NAPI_AUTO_LENGTH, desc.setter, desc.data, JS_CFUNC_setter_magic, &s_val) == napi_ok)
          {
            setter = JS_DupValue(ctx, s_val->get_inner());
          }
        }

        int accessor_flags = base_flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET;
        JS_DefinePropertyGetSet(ctx, target, key, getter, setter, accessor_flags);
      }
      else if (desc.value != nullptr)
      {
        // --- Standard Values ---
        int value_flags = base_flags | JS_PROP_HAS_WRITABLE;
        if (desc.attributes & napi_writable)
          value_flags |= JS_PROP_WRITABLE;

        JSValue val = JS_DupValue(ctx, desc.value->get_inner());
        JS_DefinePropertyValue(ctx, target, key, val, value_flags);
      }

      JS_FreeAtom(ctx, key);

      // Fail out safely if property creation failed
      if (status != napi_ok)
      {
        JS_FreeValue(ctx, proto);
        return status;
      }
    }

    JS_FreeValue(ctx, proto);
    *result = ctor_napi_value;
    return napi_ok;
  }

  napi_status napi_new_instance(napi_env env,
                                napi_value constructor,
                                size_t argc,
                                const napi_value *argv,
                                napi_value *result)
  {
    // 1. Basic validation
    if (env == nullptr || constructor == nullptr || result == nullptr)
    {
      return napi_invalid_arg;
    }

    JSContext *ctx = env->context();

    // 2. Unwrap the constructor JSValue
    // Assuming 'unwrap_value' is your helper to get the JSValue from napi_value
    JSValue ctor_val = constructor->get_inner();

    // 3. Prepare the arguments
    // QuickJS's JS_CallConstructor expects an array of JSValue.
    // Since napi_value is usually a typedef for JSValue (or a pointer to it),
    // we can often cast the array if the memory layout matches,
    // but a safe stack allocation is better for compatibility.
    JSValue *js_argv = nullptr;
    if (argc > 0)
    {
      js_argv = (JSValue *)alloca(sizeof(JSValue) * argc);
      for (size_t i = 0; i < argc; ++i)
      {
        js_argv[i] = argv[i]->get_inner();
      }
    }

    // 4. Call the constructor
    JSValue instance = JS_CallConstructor(ctx, ctor_val, (int)argc, js_argv);

    // 5. Check for exceptions
    if (JS_IsException(instance))
    {
      return ReturnPendingIfCaught(env, "Exception during constructor call");
    }

    // 6. Wrap and return
    // 'wrap_value' converts the JSValue back into the napi_value handle
    *result = env->current_scope->wrap_value(instance, true);

    return napi_ok;
  }

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

    JSValue js_func = func->get_inner();

    // 2. Ensure the target is actually a function
    if (!JS_IsFunction(env->ctx, js_func))
    {
      return napi_quickjs_set_last_error(env, napi_function_expected, "Target is not a function");
    }

    // 3. Resolve the receiver ('this' object)
    // If recv is null, Node-API defaults to 'undefined'
    JSValue js_recv = (recv != nullptr) ? recv->get_inner() : JS_UNDEFINED;

    // 4. Prepare the arguments array
    // We use a small stack buffer for performance, falling back to a heap vector for many args.
    JSValue *js_argv = nullptr;
    if (argc > 0)
    {
      // TODO: Use JSRuntime allocator, and ensure memory is freed afterwards!
      js_argv = static_cast<JSValue *>(alloca(sizeof(JSValue) * argc));
      for (size_t i = 0; i < argc; i++)
      {
        js_argv[i] = argv[i]->get_inner();
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
      // wrap the result; the current handle scope now owns the returned JSValue
      *result = env->current_scope->wrap_value(js_result, true);
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
      return InvalidArg(env);

    JSValue obj = object->get_inner();
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
        JSValue name_val = p.name->get_inner();
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
        status = MakeFunctionConstructible(env, method_fn);
        if (status != napi_ok)
        {
          JS_FreeAtom(env->ctx, prop_name);
          JS_FreeValue(env->ctx, method_fn);
          return status;
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

        JSValue value = JS_DupValue(env->ctx, p.value->get_inner());
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

  napi_status NAPI_CDECL napi_create_promise(napi_env env,
                                             napi_deferred *deferred,
                                             napi_value *promise)
  {
    if (!CheckEnv(env) || deferred == nullptr || promise == nullptr)
      return napi_invalid_arg;

    JSValue resolving_funcs[2];
    JSValue p = JS_NewPromiseCapability(env->ctx, resolving_funcs);
    if (JS_IsException(p))
      return ReturnPendingIfCaught(env, "Failed to create Promise");

    auto *d = new (std::nothrow) napi_deferred__();
    if (d == nullptr)
    {
      JS_FreeValue(env->ctx, resolving_funcs[0]);
      JS_FreeValue(env->ctx, resolving_funcs[1]);
      JS_FreeValue(env->ctx, p);
      return napi_generic_failure;
    }

    d->env = env;
    d->resolve = resolving_funcs[0];
    d->reject = resolving_funcs[1];

    *deferred = d;
    *promise = env->current_scope->wrap_value(p, true);
    return (*promise == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
                                               napi_deferred deferred,
                                               napi_value resolution)
  {
    if (!CheckEnv(env) || deferred == nullptr || resolution == nullptr)
      return napi_invalid_arg;

    JSValue arg = resolution->get_inner();
    JSValue ret = JS_Call(env->ctx, deferred->resolve, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(env->ctx, deferred->resolve);
    JS_FreeValue(env->ctx, deferred->reject);
    delete deferred;

    if (JS_IsException(ret))
      return ReturnPendingIfCaught(env, "Failed to resolve promise");

    JS_FreeValue(env->ctx, ret);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
                                              napi_deferred deferred,
                                              napi_value rejection)
  {
    if (!CheckEnv(env) || deferred == nullptr || rejection == nullptr)
      return napi_invalid_arg;

    JSValue arg = rejection->get_inner();
    JSValue ret = JS_Call(env->ctx, deferred->reject, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(env->ctx, deferred->resolve);
    JS_FreeValue(env->ctx, deferred->reject);
    delete deferred;

    if (JS_IsException(ret))
      return ReturnPendingIfCaught(env, "Failed to reject promise");

    JS_FreeValue(env->ctx, ret);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool *is_promise)
  {
    if (!CheckValue(env, value) || is_promise == nullptr)
      return napi_invalid_arg;

    *is_promise = JS_IsPromise(value->get_inner());
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
    JSValue local = object->get_inner();
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue key_value = key->get_inner();
    if (!JS_IsString(key_value) && !JS_IsSymbol(key_value))
      return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");

    JSAtom prop = JS_ValueToAtom(env->ctx, key_value);
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    // JS_SetProperty consumes value
    int res = JS_SetProperty(env->ctx, local, prop, JS_DupValue(env->ctx, value->get_inner()));
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue key_value = key->get_inner();
    if (!JS_IsString(key_value) && !JS_IsSymbol(key_value))
      return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");

    JSAtom prop = JS_ValueToAtom(env->ctx, key_value);
    if (prop == JS_ATOM_NULL)
      return ReturnPendingIfCaught(env, "Invalid key");

    JSValue out = JS_GetProperty(env->ctx, local, prop);
    JS_FreeAtom(env->ctx, prop);

    if (JS_IsException(out))
    {
      return ReturnPendingIfCaught(env, "Exception while getting property");
    }
    *result = env->current_scope->wrap_value(out, true);
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue key_value = key->get_inner();
    if (!JS_IsString(key_value) && !JS_IsSymbol(key_value))
      return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");

    JSAtom prop = JS_ValueToAtom(env->ctx, key_value);
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue key_value = key->get_inner();
    if (!JS_IsString(key_value) && !JS_IsSymbol(key_value))
      return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");

    JSAtom prop = JS_ValueToAtom(env->ctx, key_value);
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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue key_value = key->get_inner();
    if (!JS_IsString(key_value) && !JS_IsSymbol(key_value))
      return napi_quickjs_set_last_error(env, napi_name_expected, "A string or symbol was expected");

    JSAtom prop = JS_ValueToAtom(env->ctx, key_value);
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

  napi_status NAPI_CDECL napi_get_property_names(napi_env env,
                                                 napi_value object,
                                                 napi_value *result)
  {
    return GetPropertyNames(env,
                            object,
                            napi_key_include_prototypes,
                            static_cast<napi_key_filter>(napi_key_enumerable | napi_key_skip_symbols),
                            napi_key_numbers_to_strings,
                            result);
  }

  napi_status NAPI_CDECL napi_get_all_property_names(napi_env env,
                                                     napi_value object,
                                                     napi_key_collection_mode key_mode,
                                                     napi_key_filter key_filter,
                                                     napi_key_conversion key_conversion,
                                                     napi_value *result)
  {
    return GetPropertyNames(env, object, key_mode, key_filter, key_conversion, result);
  }

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

    JSValue obj = object->get_inner();

    // 2. Ensure the target is an object
    if (!JS_IsObject(obj))
    {
      return napi_quickjs_set_last_error(env, napi_object_expected, "Target is not an object");
    }

    // 3. unwrap the value to be set
    JSValue val = value->get_inner();

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
    JSValue local = object->get_inner();
    if (!JS_IsObject(local))
      return napi_object_expected;

    JSValue prop = JS_GetPropertyStr(env->ctx, local, utf8name);
    if (JS_IsException(prop))
    {
      return ReturnPendingIfCaught(env, "Exception while getting named property");
    }
    *result = env->current_scope->wrap_value(prop, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_get_prototype(napi_env env,
                                            napi_value object,
                                            napi_value *result)
  {
    if (!CheckValue(env, object) || result == nullptr)
      return InvalidArg(env);

    JSValue target = object->get_inner();
    if (!JS_IsObject(target))
      return napi_object_expected;

    JSValue proto = JS_GetPrototype(env->ctx, target);
    if (JS_IsException(proto))
      return ReturnPendingIfCaught(env, "Exception while getting prototype");

    *result = env->current_scope->wrap_value(proto, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL node_api_set_prototype(napi_env env,
                                                napi_value object,
                                                napi_value value)
  {
    if (!CheckValue(env, object) || !CheckValue(env, value))
      return napi_invalid_arg;

    JSValue target = object->get_inner();
    if (!JS_IsObject(target))
      return napi_object_expected;

    if (JS_SetPrototype(env->ctx, target, value->get_inner()) < 0)
      return ReturnPendingIfCaught(env, "Exception while setting prototype");

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_bool(napi_env env,
                                             napi_value value,
                                             bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
    {
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValue local = value->get_inner();
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
    JSValue local = value->get_inner();
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
      else
      {
        JS_FreeCString(env->ctx, str);
        return InvalidArg(env);
      }
    }
    else if (bufsize != 0)
    {
      size_t copied = CompleteUtf8PrefixLength(str, std::min(bufsize - 1, len));
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

  napi_status NAPI_CDECL napi_get_value_string_latin1(
      napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result)
  {
    if (!CheckValue(env, value))
      return InvalidArg(env);

    JSValue local = value->get_inner();
    if (!JS_IsString(local))
      return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");

    // QuickJS strings are UTF-8 internally; for latin1 we get the UTF-8
    // and truncate to single bytes (values >0xFF become '?').
    size_t len;
    const char *str = JS_ToCStringLen(env->ctx, &len, local);
    if (str == nullptr)
      return ReturnPendingIfCaught(env, "Failed to convert string");

    std::vector<char> latin1 = Utf8ToLatin1(str, len);

    if (buf == nullptr)
    {
      if (result == nullptr)
      {
        JS_FreeCString(env->ctx, str);
        return InvalidArg(env);
      }
      *result = latin1.size();
    }
    else if (bufsize != 0)
    {
      size_t copy_len = std::min(bufsize - 1, latin1.size());
      memcpy(buf, latin1.data(), copy_len);
      buf[copy_len] = '\0';
      if (result != nullptr)
        *result = copy_len;
    }
    else if (result != nullptr)
    {
      *result = 0;
    }

    JS_FreeCString(env->ctx, str);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env,
                                                     napi_value value,
                                                     char16_t *buf,
                                                     size_t bufsize,
                                                     size_t *result)
  {
    if (!CheckValue(env, value))
      return InvalidArg(env);

    JSValue local = value->get_inner();
    if (!JS_IsString(local))
      return napi_quickjs_set_last_error(env, napi_string_expected, "A string was expected");

    size_t utf16_len;
    const uint16_t *utf16 = JS_ToCStringLenUTF16(env->ctx, &utf16_len, local);
    if (utf16 == nullptr)
      return ReturnPendingIfCaught(env, "Failed to convert string to UTF-16");

    if (buf == nullptr)
    {
      if (result == nullptr)
      {
        JS_FreeCString(env->ctx, reinterpret_cast<const char *>(utf16));
        return InvalidArg(env);
      }
      *result = utf16_len;
    }
    else if (bufsize != 0)
    {
      size_t copy_len = std::min(bufsize - 1, utf16_len);
      memcpy(buf, utf16, copy_len * sizeof(char16_t));
      buf[copy_len] = u'\0';
      if (result != nullptr)
        *result = copy_len;
    }
    else if (result != nullptr)
    {
      *result = 0;
    }

    JS_FreeCString(env->ctx, reinterpret_cast<const char *>(utf16));
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value, napi_value *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);

    JSValue coerced = JS_ToBoolean(env->ctx, value->get_inner());
    if (JS_IsException(coerced))
      return ReturnPendingIfCaught(env, "Failed to coerce to bool");

    *result = env->current_scope->wrap_value(coerced, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool *result)
  {
    if (!CheckValue(env, lhs) || !CheckValue(env, rhs) || result == nullptr)
      return napi_invalid_arg;

    *result = JS_IsStrictEqual(env->ctx, lhs->get_inner(), rhs->get_inner());
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_coerce_to_number(napi_env env, napi_value value, napi_value *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);

    double d;
    if (JS_ToFloat64(env->ctx, &d, value->get_inner()) != 0)
      return ReturnPendingIfCaught(env, "Failed to coerce to number");

    *result = env->current_scope->wrap_value(JS_NewFloat64(env->ctx, d), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_coerce_to_object(napi_env env, napi_value value, napi_value *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);

    JSValue obj = JS_ToObject(env->ctx, value->get_inner());
    if (JS_IsException(obj))
      return ReturnPendingIfCaught(env, "Failed to coerce to object");

    *result = env->current_scope->wrap_value(obj, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_coerce_to_string(napi_env env, napi_value value, napi_value *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);

    JSValue str = JS_ToString(env->ctx, value->get_inner());
    if (JS_IsException(str))
      return ReturnPendingIfCaught(env, "Failed to coerce to string");

    *result = env->current_scope->wrap_value(str, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value, uint32_t initial_ref_count, napi_ref *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;

    *result = new (std::nothrow)
        napi_ref__(env, value->get_inner(), initial_ref_count);
    if (*result == nullptr)
      return napi_generic_failure;

    TrackWeakRef(env, *result);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_delete_reference(napi_env env, napi_ref ref)
  {
    if (!CheckEnv(env) || ref == nullptr)
      return napi_invalid_arg;

    RemoveWeakRef(env, ref);

    if (ref->ref_count > 0)
      JS_FreeValue(env->ctx, ref->value);

    delete ref;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_reference_ref(napi_env env,
                                            napi_ref ref,
                                            uint32_t *result)
  {
    if (!CheckEnv(env) || ref == nullptr)
      return napi_invalid_arg;

    if (!IsEmptyRef(ref))
    {
      // first time we increment internal reference we're becoming
      // an owner and napi_ref becomes strong.
      if (ref->ref_count == 0)
      {
        RemoveWeakRef(env, ref);
        // we only increment internal count once
        JS_DupValue(env->ctx, ref->value);
      }

      // we need to track our own reference count independently from
      // internal QuickJS one, because we need to return it in result.
      ref->ref_count++;
    }

    if (result != nullptr)
      *result = ref->ref_count;

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_reference_unref(napi_env env,
                                              napi_ref ref,
                                              uint32_t *result)
  {
    if (!CheckEnv(env) || ref == nullptr)
      return napi_invalid_arg;

    // should only drop internal ref-count if
    // we have incremented it in the first place
    if (ref->ref_count > 0)
    {
      ref->ref_count--;

      // when count reaches 0 it becomes weak-reference,
      // and is elligible for GC collection.
      if (ref->ref_count == 0)
      {
        TrackWeakRef(env, ref);
        JS_FreeValue(env->ctx, ref->value);
        if (!ref->can_be_weak)
          ref->value = JS_UNDEFINED;
      }
    }

    if (result != nullptr)
      *result = ref->ref_count;

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_reference_value(napi_env env,
                                                  napi_ref ref,
                                                  napi_value *result)
  {
    if (!CheckEnv(env) || ref == nullptr || result == nullptr)
      return napi_invalid_arg;
    if (IsEmptyRef(ref))
    {
      *result = nullptr;
      return napi_ok;
    }
    *result = env->current_scope->wrap_value(JS_DupValue(env->ctx, ref->value), true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status napi_wrap(napi_env env, napi_value js_object,
                        void *native_object, napi_finalize finalize_cb,
                        void *finalize_hint, napi_ref *result)
  {
    if (!CheckValue(env, js_object))
      return napi_invalid_arg;

    auto obj = js_object->get_inner();
    if (!JS_IsObject(obj))
      return napi_object_expected;

    if (GetWrapRecord(env->ctx, obj) != nullptr)
      return InvalidArg(env);

    auto *wrap = new (std::nothrow) napi_external_backing_store_hint__{
        .env = env,
        .external_data = native_object,
        .finalize_cb = finalize_cb,
        .finalize_hint = finalize_hint};
    if (wrap == nullptr)
    {
      return napi_generic_failure;
    }

    if (JS_SetOpaque(obj, wrap) != 0)
    {
      JSValue stored = JS_NewObjectClass(env->ctx, napi_external_class_id);
      if (JS_IsException(stored))
      {
        delete wrap;
        return ReturnPendingIfCaught(env, "Failed to create wrap record");
      }
      wrap->weak_target = obj;
      JS_SetOpaque(stored, wrap);
      if (JS_DefinePropertyValueStr(env->ctx, obj, kWrapProperty, stored,
                                    JS_PROP_CONFIGURABLE) < 0)
      {
        wrap->weak_target = JS_UNDEFINED;
        JS_SetOpaque(stored, nullptr);
        JS_FreeValue(env->ctx, stored);
        delete wrap;
        return ReturnPendingIfCaught(env, "Failed to attach wrap record");
      }
    }

    if (result != nullptr)
      return napi_create_reference(env, js_object, 1, result);

    return napi_ok;
  }

  napi_status napi_unwrap(napi_env env, napi_value js_object, void **result)
  {
    if (!CheckValue(env, js_object) || result == nullptr)
      return napi_invalid_arg;

    auto obj = js_object->get_inner();
    auto *wrap = GetWrapRecord(env->ctx, obj);
    if (wrap == nullptr)
      return napi_invalid_arg;

    *result = wrap->external_data;
    return napi_ok;
  }

  napi_status napi_remove_wrap(napi_env env, napi_value js_object, void **result)
  {
    if (!CheckValue(env, js_object) || result == nullptr)
      return napi_invalid_arg;

    auto obj = js_object->get_inner();
    if (!JS_IsObject(obj))
      return napi_object_expected;

    auto *wrap = GetWrapRecord(env->ctx, obj);
    if (wrap == nullptr)
      return napi_generic_failure;

    *result = wrap->external_data;

    if (JS_SetOpaque(obj, nullptr) != 0)
    {
      JSValue stored = JS_GetPropertyStr(env->ctx, obj, kWrapProperty);
      if (JS_IsException(stored))
      {
        JSValue exc = JS_GetException(env->ctx);
        JS_FreeValue(env->ctx, exc);
        return ReturnPendingIfCaught(env, "Failed to get wrap record");
      }
      if (!JS_IsUndefined(stored))
      {
        JS_SetOpaque(stored, nullptr);
      }

      JSAtom key = JS_NewAtom(env->ctx, kWrapProperty);
      if (JS_DeleteProperty(env->ctx, obj, key, 0) < 0)
      {
        if (!JS_IsUndefined(stored))
        {
          JS_SetOpaque(stored, wrap);
        }
        JS_FreeValue(env->ctx, stored);
        JS_FreeAtom(env->ctx, key);
        return ReturnPendingIfCaught(env, "Failed to remove wrap record");
      }
      JS_FreeValue(env->ctx, stored);
      JS_FreeAtom(env->ctx, key);
    }

    delete wrap;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_throw_error(napi_env env,
                                          const char *code,
                                          const char *msg)
  {
    if (!CheckEnv(env))
    {
      return napi_invalid_arg;
    }

    JSValue error = CreatePlainError(env->ctx, msg);
    if (code != nullptr)
    {
      JS_SetPropertyStr(env->ctx, error, "code",
                        JS_NewString(env->ctx, code));
    }

    SetLastException(env, error);

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error)
  {
    if (!CheckValue(env, error))
      return napi_invalid_arg;
    SetLastException(env, JS_DupValue(env->ctx, error->get_inner()));
    return napi_pending_exception;
  }

  napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return napi_invalid_arg;
    JSValue val = value->get_inner();
    *result = JS_IsObject(val) &&
              JS_HasProperty(env->ctx, val, JS_ATOM_message) &&
              JS_HasProperty(env->ctx, val, JS_ATOM_stack);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char *code, const char *msg)
  {
    if (!CheckEnv(env))
      return napi_invalid_arg;
    SetLastException(env, CreateErrorObject(env->ctx, JS_NewTypeError, code, msg));
    return napi_pending_exception;
  }

  napi_status NAPI_CDECL napi_throw_range_error(napi_env env, const char *code, const char *msg)
  {
    if (!CheckEnv(env))
      return napi_invalid_arg;
    SetLastException(env, CreateErrorObject(env->ctx, JS_NewRangeError, code, msg));
    return napi_pending_exception;
  }

  napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env, const char *code, const char *msg)
  {
    if (!CheckEnv(env))
      return napi_invalid_arg;
    SetLastException(env, CreateErrorObject(env->ctx, JS_NewSyntaxError, code, msg));
    return napi_pending_exception;
  }

  napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err)
  {
    if (!CheckEnv(env) || err == nullptr)
      return napi_invalid_arg;

    SetLastException(env, JS_DupValue(env->ctx, err->get_inner()));
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value *result)
  {
    return CreatePlainErrorCommon(env, code, msg, result);
  }

  napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value *result)
  {
    return CreateErrorCommon(env, JS_NewTypeError, code, msg, result);
  }

  napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value *result)
  {
    return CreateErrorCommon(env, JS_NewRangeError, code, msg, result);
  }

  napi_status NAPI_CDECL node_api_create_syntax_error(napi_env env, napi_value code, napi_value msg, napi_value *result)
  {
    return CreateErrorCommon(env, JS_NewSyntaxError, code, msg, result);
  }

  napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool *result)
  {
    if (!CheckEnv(env) || result == nullptr)
    {
      return napi_invalid_arg;
    }

    // Check both the engine state and our internal environment cache
    *result = JS_HasException(env->ctx) || env->has_last_exception;

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env, napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;

    if (!env->has_last_exception)
      return napi_generic_failure;

    // Take ownership of the exception before clearing
    JSValue ex = env->last_exception;
    env->last_exception = JS_UNDEFINED;
    env->has_last_exception = false;

    *result = env->current_scope->wrap_value(ex, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status napi_set_instance_data(napi_env env,
                                     void *data,
                                     napi_finalize finalize_cb,
                                     void *finalize_hint)
  {
    if (env == nullptr)
      return napi_invalid_arg;

    // Node-API Requirement: If instance data was previously set,
    // the previous data's finalizer should be called before overwriting.
    if (env->instance_data != nullptr && env->instance_data_finalize_cb != nullptr)
    {
      env->instance_data_finalize_cb(env, env->instance_data, env->instance_data_finalize_hint);
    }

    env->instance_data = data;
    env->instance_data_finalize_cb = finalize_cb;
    env->instance_data_finalize_hint = finalize_hint;

    return napi_ok;
  }

  napi_status napi_get_instance_data(napi_env env, void **data)
  {
    if (env == nullptr || data == nullptr)
      return napi_invalid_arg;

    *data = env->instance_data;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_run_script(napi_env env,
                                         napi_value script,
                                         napi_value *result)
  {
    if (!CheckValue(env, script) || result == nullptr)
      return napi_invalid_arg;

    JSValue source = script->get_inner();
    if (!JS_IsString(source))
      return napi_string_expected;

    size_t len;
    const char *str = JS_ToCStringLen(env->ctx, &len, source);
    if (str == nullptr)
      return ReturnPendingIfCaught(env, "Failed to convert script to string");

    JSValue out = JS_Eval(env->ctx, str, len, "<napi_run_script>", JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(out))
    {
      JSValue exc = JS_GetException(env->ctx);
      if (JS_IsObject(exc))
      {
        JSAtom arrow_atom = JS_NewAtom(env->ctx, "node:arrowMessage");
        int has_arrow = JS_HasProperty(env->ctx, exc, arrow_atom);
        if (has_arrow == 0)
        {
          JS_SetProperty(env->ctx, exc, arrow_atom, JS_NewStringLen(env->ctx, str, len));
        }
        JS_FreeAtom(env->ctx, arrow_atom);
      }
      JS_FreeCString(env->ctx, str);
      SetLastException(env, exc);
      return napi_quickjs_set_last_error(env, napi_pending_exception, "Script evaluation failed");
    }

    JS_FreeCString(env->ctx, str);

    *result = env->current_scope->wrap_value(out, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
      napi_env env,
      napi_value arraybuffer,
      size_t byte_offset,
      size_t byte_length,
      napi_value *result);

  napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env basic_env,
                                                   napi_cleanup_hook fun,
                                                   void *arg)
  {
    auto *env = const_cast<napi_env>(basic_env);
    if (!CheckEnv(env) || fun == nullptr)
      return napi_invalid_arg;

    auto *entry = new (std::nothrow) napi_env_cleanup_hook__();
    if (entry == nullptr)
      return napi_generic_failure;
    entry->hook = fun;
    entry->arg = arg;
    env->env_cleanup_hooks.push_back(entry);
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env basic_env,
                                                      napi_cleanup_hook fun,
                                                      void *arg)
  {
    auto *env = const_cast<napi_env>(basic_env);
    if (!CheckEnv(env) || fun == nullptr)
      return napi_invalid_arg;

    auto &hooks = env->env_cleanup_hooks;
    for (auto it = hooks.begin(); it != hooks.end(); ++it)
    {
      auto *entry = *it;
      if (entry != nullptr && entry->hook == fun && entry->arg == arg)
      {
        delete entry;
        hooks.erase(it);
        return napi_ok;
      }
    }
    return napi_invalid_arg;
  }

  napi_status NAPI_CDECL napi_create_buffer(napi_env env,
                                            size_t length,
                                            void **data,
                                            napi_value *result)
  {
    if (!CheckEnv(env) || data == nullptr || result == nullptr)
      return InvalidArg(env);

    napi_value arraybuffer = nullptr;
    napi_status status = napi_create_arraybuffer(env, length, data, &arraybuffer);
    if (status != napi_ok)
      return status;

    status = node_api_create_buffer_from_arraybuffer(env, arraybuffer, 0, length, result);
    return status;
  }

  napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env,
                                                 size_t length,
                                                 const void *data,
                                                 void **result_data,
                                                 napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr || (length > 0 && data == nullptr))
      return InvalidArg(env);

    void *out = nullptr;
    napi_status status = napi_create_buffer(env, length, &out, result);
    if (status != napi_ok)
      return status;

    if (length > 0)
      std::memcpy(out, data, length);
    if (result_data != nullptr)
      *result_data = out;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_create_external_buffer(napi_env env,
                                                     size_t length,
                                                     void *data,
                                                     node_api_basic_finalize finalize_cb,
                                                     void *finalize_hint,
                                                     napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr || (length > 0 && data == nullptr))
      return InvalidArg(env);

    napi_value arraybuffer = nullptr;
    napi_status status = napi_create_external_arraybuffer(
        env, data, length, finalize_cb, finalize_hint, &arraybuffer);
    if (status != napi_ok)
      return status;

    return node_api_create_buffer_from_arraybuffer(env, arraybuffer, 0, length, result);
  }

  napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool *result)
  {
    if (!CheckValue(env, value) || result == nullptr)
      return InvalidArg(env);
    *result = IsBufferValue(env, value->get_inner());
    return napi_quickjs_clear_last_error(env);
  }

  napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
                                              napi_value value,
                                              void **data,
                                              size_t *length)
  {
    if (!CheckValue(env, value))
      return InvalidArg(env);
    return GetBufferInfo(env, value->get_inner(), data, length);
  }

  napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
      napi_env env,
      napi_value arraybuffer,
      size_t byte_offset,
      size_t byte_length,
      napi_value *result)
  {
    if (!CheckValue(env, arraybuffer) || result == nullptr)
      return InvalidArg(env);

    size_t arraybuffer_length = 0;
    uint8_t *arraybuffer_data = JS_GetArrayBuffer(
        env->ctx, &arraybuffer_length, arraybuffer->get_inner());
    if (arraybuffer_data == nullptr && JS_HasException(env->ctx))
    {
      JSValue exc = JS_GetException(env->ctx);
      JS_FreeValue(env->ctx, exc);
      return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    if (byte_offset > arraybuffer_length || byte_length > arraybuffer_length - byte_offset)
      return InvalidArg(env);

    JSValue argv[] = {
        arraybuffer->get_inner(),
        JS_NewInt64(env->ctx, static_cast<int64_t>(byte_offset)),
        JS_NewInt64(env->ctx, static_cast<int64_t>(byte_length))};
    JSValue buffer = JS_NewTypedArray(env->ctx, 3, argv, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(env->ctx, argv[1]);
    JS_FreeValue(env->ctx, argv[2]);
    if (JS_IsException(buffer))
      return ReturnPendingIfCaught(env, "Failed to create Buffer");

    napi_status status = MarkBuffer(env, buffer);
    if (status != napi_ok)
    {
      JS_FreeValue(env->ctx, buffer);
      return status;
    }

    *result = env->current_scope->wrap_value(buffer, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

  napi_status NAPI_CDECL napi_adjust_external_memory(
      node_api_basic_env basic_env, int64_t change_in_bytes, int64_t *adjusted_value)
  {
    auto *env = const_cast<napi_env>(basic_env);
    if (!CheckEnv(env) || adjusted_value == nullptr)
      return napi_invalid_arg;

    if (change_in_bytes > 0 &&
        env->external_memory > std::numeric_limits<int64_t>::max() - change_in_bytes)
    {
      env->external_memory = std::numeric_limits<int64_t>::max();
    }
    else if (change_in_bytes < 0 &&
             env->external_memory < std::numeric_limits<int64_t>::min() - change_in_bytes)
    {
      env->external_memory = std::numeric_limits<int64_t>::min();
    }
    else
    {
      env->external_memory += change_in_bytes;
    }

    *adjusted_value = env->external_memory;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_add_finalizer(napi_env env,
                                            napi_value js_object,
                                            void *finalize_data,
                                            node_api_basic_finalize finalize_cb,
                                            void *finalize_hint,
                                            napi_ref *result)
  {
    if (!CheckValue(env, js_object) || finalize_cb == nullptr)
      return InvalidArg(env);

    JSValue obj = js_object->get_inner();
    if (!JS_IsObject(obj))
      return napi_object_expected;

    auto *hint = new (std::nothrow) napi_external_backing_store_hint__{
        .env = env,
        .external_data = finalize_data,
        .finalize_cb = finalize_cb,
        .finalize_hint = finalize_hint,
        .weak_target = obj};
    if (hint == nullptr)
      return napi_generic_failure;

    JSValue stored = JS_NewObjectClass(env->ctx, napi_external_class_id);
    if (JS_IsException(stored))
    {
      delete hint;
      return ReturnPendingIfCaught(env, "Failed to create finalizer record");
    }

    JS_SetOpaque(stored, hint);
    if (JS_DefinePropertyValueStr(env->ctx, obj, kFinalizerProperty, stored,
                                  JS_PROP_CONFIGURABLE) < 0)
    {
      JS_SetOpaque(stored, nullptr);
      JS_FreeValue(env->ctx, stored);
      delete hint;
      return ReturnPendingIfCaught(env, "Failed to attach finalizer record");
    }

    if (result != nullptr)
      return napi_create_reference(env, js_object, 0, result);

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_get_version(node_api_basic_env env, uint32_t *result)
  {
    if (result == nullptr)
      return napi_invalid_arg;
    auto *napiEnv = const_cast<napi_env>(env);
    if (!CheckEnv(napiEnv))
      return napi_invalid_arg;
    *result = 10;
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object)
  {
    if (!CheckValue(env, object))
      return napi_invalid_arg;

    JSValue target = object->get_inner();
    if (!JS_IsObject(target))
      return napi_object_expected;

    if (JS_FreezeObject(env->ctx, target) < 0)
      return ReturnPendingIfCaught(env, "Failed to freeze object");

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object)
  {
    if (!CheckValue(env, object))
      return napi_invalid_arg;

    JSValue target = object->get_inner();
    if (!JS_IsObject(target))
      return napi_object_expected;

    if (JS_SealObject(env->ctx, target) < 0)
      return ReturnPendingIfCaught(env, "Failed to seal object");

    return napi_ok;
  }

  napi_status NAPI_CDECL napi_type_tag_object(napi_env env,
                                              napi_value value,
                                              const napi_type_tag *type_tag)
  {
    if (!CheckValue(env, value) || type_tag == nullptr)
      return napi_invalid_arg;

    JSValue target = value->get_inner();
    if (!JS_IsObject(target))
      return napi_invalid_arg;

    // Encode the two 64-bit halves as a single JS string "lower:upper"
    // so we don't need a separate allocation.
    char buf[64];
    snprintf(buf, sizeof(buf), "%llu:%llu",
             (unsigned long long)type_tag->lower,
             (unsigned long long)type_tag->upper);

    JS_DefinePropertyValueStr(env->ctx, target, kTypeTagProperty,
                              JS_NewString(env->ctx, buf),
                              JS_PROP_C_W_E); // not enumerable would be nicer but this is simplest
    return napi_ok;
  }

  napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env,
                                                    napi_value value,
                                                    const napi_type_tag *type_tag,
                                                    bool *result)
  {
    if (!CheckValue(env, value) || type_tag == nullptr || result == nullptr)
      return napi_invalid_arg;

    *result = false;

    JSValue target = value->get_inner();
    if (!JS_IsObject(target))
      return napi_ok;

    JSValue tag_val = JS_GetPropertyStr(env->ctx, target, kTypeTagProperty);
    if (!JS_IsString(tag_val))
    {
      JS_FreeValue(env->ctx, tag_val);
      return napi_ok;
    }

    const char *stored = JS_ToCString(env->ctx, tag_val);
    JS_FreeValue(env->ctx, tag_val);

    char expected[64];
    snprintf(expected, sizeof(expected), "%llu:%llu",
             (unsigned long long)type_tag->lower,
             (unsigned long long)type_tag->upper);

    *result = (strcmp(stored, expected) == 0);
    JS_FreeCString(env->ctx, stored);
    return napi_ok;
  }

  napi_status NAPI_CDECL node_api_create_object_with_properties(napi_env env,
                                                                napi_value prototype_or_null,
                                                                napi_value *property_names,
                                                                napi_value *property_values,
                                                                size_t property_count,
                                                                napi_value *result)
  {
    if (!CheckEnv(env) || result == nullptr)
      return napi_invalid_arg;
    if (property_count > 0 && (property_names == nullptr || property_values == nullptr))
      return napi_invalid_arg;

    JSContext *ctx = env->ctx;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
      return ReturnPendingIfCaught(env, "Failed to create object");

    if (prototype_or_null != nullptr)
    {
      JSValue proto = prototype_or_null->get_inner();
      if (!JS_IsNull(proto) && !JS_IsObject(proto))
      {
        JS_FreeValue(ctx, obj);
        return napi_object_expected;
      }
      if (JS_SetPrototype(ctx, obj, proto) < 0)
      {
        JS_FreeValue(ctx, obj);
        return ReturnPendingIfCaught(env, "Failed to set prototype");
      }
    }

    for (size_t i = 0; i < property_count; ++i)
    {
      if (property_names[i] == nullptr || property_values[i] == nullptr)
      {
        JS_FreeValue(ctx, obj);
        return napi_invalid_arg;
      }

      JSAtom key = JS_ValueToAtom(ctx, property_names[i]->get_inner());
      JSValue val = JS_DupValue(ctx, property_values[i]->get_inner());
      int rc = JS_SetProperty(ctx, obj, key, val);
      JS_FreeAtom(ctx, key);

      if (rc < 0)
      {
        JS_FreeValue(ctx, obj);
        return ReturnPendingIfCaught(env, "Failed to set property");
      }
    }

    *result = env->current_scope->wrap_value(obj, true);
    return (*result == nullptr) ? napi_generic_failure : napi_ok;
  }

} // extern "C"
