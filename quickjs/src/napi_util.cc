#include "internal/napi_util.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

#include <cstring>

bool napi_util__::check_env(napi_env env)
{
  return env != nullptr && env->context() != nullptr;
}

bool napi_util__::check_value(napi_env env, napi_value value)
{
  return check_env(env) && value != nullptr;
}

void napi_util__::clear_last_exception(napi_env env)
{
  if (env != nullptr)
    env->clear_last_exception();
}

void napi_util__::set_last_exception(napi_env env, JSValue exception)
{
  if (env != nullptr)
    env->set_last_exception(exception);
}

bool napi_util__::rethrow_last_exception(napi_env env, JSContext *ctx)
{
  if (!env->has_last_exception())
    return false;

  JS_Throw(ctx, env->take_last_exception());
  return true;
}

napi_status napi_util__::return_pending_if_caught(napi_env env, const char *message)
{
  if (JS_HasException(env->context()))
  {
    auto exc = JS_GetException(env->context());
    set_last_exception(env, exc);
    return napi_quickjs_set_last_error(env, napi_pending_exception, message);
  }
  return napi_quickjs_set_last_error(env, napi_generic_failure, message);
}

napi_status napi_util__::invalid_arg(napi_env env)
{
  if (check_env(env))
    return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  return napi_invalid_arg;
}

bool napi_util__::decimal_digits_fit(const char *value, const char *max)
{
  while (*value == '0' && value[1] != '\0')
    ++value;
  size_t value_len = std::strlen(value);
  size_t max_len = std::strlen(max);
  if (value_len != max_len)
    return value_len < max_len;
  return std::strcmp(value, max) <= 0;
}

bool napi_util__::bigint_fits_signed64(JSContext *ctx, JSValueConst value)
{
  const char *str = JS_ToCString(ctx, value);
  if (str == nullptr)
    return false;
  bool negative = str[0] == '-';
  bool fits = decimal_digits_fit(negative ? str + 1 : str,
                                 negative ? "9223372036854775808" : "9223372036854775807");
  JS_FreeCString(ctx, str);
  return fits;
}

bool napi_util__::bigint_fits_unsigned64(JSContext *ctx, JSValueConst value)
{
  const char *str = JS_ToCString(ctx, value);
  if (str == nullptr)
    return false;
  bool fits = str[0] != '-' && decimal_digits_fit(str, "18446744073709551615");
  JS_FreeCString(ctx, str);
  return fits;
}

std::vector<uint64_t> napi_util__::bigint_words_from_decimal(JSContext *ctx, JSValueConst value, bool *negative)
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

std::vector<char> napi_util__::utf8_to_latin1(const char *str, size_t len)
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

size_t napi_util__::complete_utf8_prefix_length(const char *str, size_t len)
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

JSTypedArrayEnum napi_util__::to_quickjs_array_type(napi_typedarray_type type)
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

bool napi_util__::from_quickjs_array_type(int type, napi_typedarray_type *out)
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

void napi_util__::free_array_buffer_data(JSRuntime *rt, void *opaque, void *ptr)
{
  (void)opaque;
  js_free_rt(rt, ptr);
}

int napi_util__::key_filter_to_gpn(napi_key_filter key_filter)
{
  int flags = 0;
  if (!(key_filter & napi_key_skip_strings))
    flags |= JS_GPN_STRING_MASK;
  if (!(key_filter & napi_key_skip_symbols))
    flags |= JS_GPN_SYMBOL_MASK;
  if (key_filter & napi_key_enumerable)
    flags |= JS_GPN_ENUM_ONLY;
  return flags;
}

napi_status napi_util__::get_property_names(napi_env env,
                                            napi_value object,
                                            napi_key_collection_mode key_mode,
                                            napi_key_filter key_filter,
                                            napi_key_conversion key_conversion,
                                            napi_value *result)
{
  if (!check_value(env, object) || result == nullptr)
    return invalid_arg(env);

  JSContext *ctx = env->context();
  JSValue obj = object->get_inner();
  if (!JS_IsObject(obj))
    return napi_object_expected;

  int gpn_flags = key_filter_to_gpn(key_filter);
  JSPropertyEnum *tab = nullptr;
  uint32_t tab_count = 0;

  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_count, obj, gpn_flags) < 0)
    return return_pending_if_caught(env, "Exception while getting property names");

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
        return return_pending_if_caught(env, "Exception while filtering property names");
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
        key = JS_AtomToValue(ctx, t[i].atom);
      }
      if (JS_IsException(key))
      {
        JS_FreePropertyEnum(ctx, t, count);
        return return_pending_if_caught(env, "Failed to convert property name");
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

  *result = env->current_scope()->wrap_value(arr, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

JSValue napi_util__::create_plain_error(JSContext *ctx, const char *msg)
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

napi_status napi_util__::create_plain_error_common(napi_env env,
                                                   napi_value code,
                                                   napi_value msg,
                                                   napi_value *result)
{
  if (!check_env(env) || msg == nullptr || result == nullptr)
    return napi_invalid_arg;

  JSValue msg_val = msg->get_inner();
  if (!JS_IsString(msg_val))
    return napi_string_expected;

  const char *msg_str = JS_ToCString(env->context(), msg_val);
  JSValue error = create_plain_error(env->context(), msg_str);
  JS_FreeCString(env->context(), msg_str);

  if (code != nullptr)
  {
    const char *code_str = JS_ToCString(env->context(), code->get_inner());
    JS_SetPropertyStr(env->context(), error, "code", JS_NewString(env->context(), code_str));
    JS_FreeCString(env->context(), code_str);
  }

  *result = env->current_scope()->wrap_value(error, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

JSValue napi_util__::create_error_object(JSContext *ctx,
                                         JSValue (*factory)(JSContext *, const char *, ...),
                                         const char *code,
                                         const char *msg)
{
  JSValue error = factory(ctx, "%s", msg ? msg : "");
  JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, msg ? msg : ""));
  if (code != nullptr)
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
  return error;
}

napi_status napi_util__::create_error_common(napi_env env,
                                             JSValue (*factory)(JSContext *, const char *, ...),
                                             napi_value code,
                                             napi_value msg,
                                             napi_value *result)
{
  if (!check_env(env) || msg == nullptr || result == nullptr)
    return napi_invalid_arg;

  JSValue msg_val = msg->get_inner();
  if (!JS_IsString(msg_val))
    return napi_string_expected;

  const char *msg_str = JS_ToCString(env->context(), msg_val);
  const char *code_str = nullptr;
  if (code != nullptr)
    code_str = JS_ToCString(env->context(), code->get_inner());

  JSValue error = create_error_object(env->context(), factory, code_str, msg_str);

  JS_FreeCString(env->context(), msg_str);
  if (code_str != nullptr)
    JS_FreeCString(env->context(), code_str);

  *result = env->current_scope()->wrap_value(error, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}
