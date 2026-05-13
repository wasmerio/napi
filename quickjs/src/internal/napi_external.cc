#include "internal/napi_external.h"

#include "internal/napi_env.h"
#include "internal/napi_util.h"

namespace
{
JSClassID external_class_id = 0;

const char k_type_tag_property[] = "__napi_type_tag__";
const char k_wrap_property[] = "__napi_wrap__";
const char k_buffer_property[] = "__napi_buffer__";
const char k_finalizer_property[] = "__napi_finalizer__";
} // namespace

int napi_external__::register_class(JSRuntime *rt)
{
  JS_NewClassID(rt, &external_class_id);
  JSClassDef def = {};
  def.class_name = "NapiExternal";
  def.finalizer = finalizer;
  return JS_NewClass(rt, external_class_id, &def);
}

JSClassID napi_external__::class_id()
{
  return external_class_id;
}

void *napi_external__::get_value(JSValueConst value)
{
  auto *hint = static_cast<napi_external_backing_store_hint *>(
      JS_GetOpaque(value, external_class_id));
  return hint == nullptr ? nullptr : hint->external_data();
}

napi_external_backing_store_hint__ *napi_external__::get_wrap_record(JSContext *ctx, JSValueConst object)
{
  auto *wrap = static_cast<napi_external_backing_store_hint__ *>(
      JS_GetOpaque(object, external_class_id));
  if (wrap != nullptr)
    return wrap;

  JSValue stored = JS_GetPropertyStr(ctx, object, k_wrap_property);
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
      JS_GetOpaque(stored, external_class_id));
  JS_FreeValue(ctx, stored);
  return wrap;
}

const char *napi_external__::type_tag_property()
{
  return k_type_tag_property;
}

const char *napi_external__::wrap_property()
{
  return k_wrap_property;
}

const char *napi_external__::finalizer_property()
{
  return k_finalizer_property;
}

napi_status napi_external__::mark_buffer(napi_env env, JSValue value)
{
  if (JS_DefinePropertyValueStr(env->context(), value, k_buffer_property,
                                JS_NewBool(env->context(), true),
                                JS_PROP_CONFIGURABLE) < 0)
    return napi_util__::return_pending_if_caught(env, "Failed to mark Buffer");
  return napi_ok;
}

bool napi_external__::is_buffer(napi_env env, JSValueConst value)
{
  if (!JS_IsObject(value))
    return false;
  JSValue marker = JS_GetPropertyStr(env->context(), value, k_buffer_property);
  if (JS_IsException(marker))
  {
    JSValue exc = JS_GetException(env->context());
    JS_FreeValue(env->context(), exc);
    return false;
  }
  bool is_buffer = JS_ToBool(env->context(), marker) == 1;
  JS_FreeValue(env->context(), marker);
  return is_buffer;
}

napi_status napi_external__::get_buffer_info(napi_env env, JSValueConst value, void **data, size_t *length)
{
  if (!is_buffer(env, value))
    return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");

  size_t offset = 0;
  size_t byte_len = 0;
  JSValue arraybuffer = JS_GetTypedArrayBuffer(env->context(), value, &offset, &byte_len, nullptr);
  if (JS_IsException(arraybuffer))
    return napi_util__::return_pending_if_caught(env, "Failed to get Buffer backing store");

  size_t arraybuffer_len = 0;
  uint8_t *arraybuffer_data = JS_GetArrayBuffer(env->context(), &arraybuffer_len, arraybuffer);
  JS_FreeValue(env->context(), arraybuffer);
  if (arraybuffer_data == nullptr && JS_HasException(env->context()))
  {
    JSValue exc = JS_GetException(env->context());
    JS_FreeValue(env->context(), exc);
    return napi_quickjs_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  if (data != nullptr)
    *data = arraybuffer_data == nullptr ? nullptr : arraybuffer_data + offset;
  if (length != nullptr)
    *length = byte_len;
  return napi_quickjs_clear_last_error(env);
}

void napi_external__::free_external_array_buffer_data(JSRuntime *rt, void *opaque, void *ptr)
{
  (void)ptr;
  auto *hint = reinterpret_cast<napi_external_backing_store_hint *>(opaque);
  if (hint == nullptr)
    return;
  hint->invoke_finalizer();
  if (hint->is_detaching())
    return;
  napi_external_backing_store_hint__::destroy_with_runtime(rt, hint);
}

void napi_external__::finalizer(JSRuntime *rt, JSValue value)
{
  auto *hint = static_cast<napi_external_backing_store_hint *>(JS_GetOpaque(value, external_class_id));
  if (hint == nullptr)
    return;

  hint->invoke_finalizer();
  napi_external_backing_store_hint__::destroy_with_runtime(rt, hint);
}
