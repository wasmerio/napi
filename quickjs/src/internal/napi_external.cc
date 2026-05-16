#include "internal/napi_external.h"

#include "internal/napi_env.h"
#include "internal/napi_util.h"

namespace
{
JSClassID external_class_id = 0;

const char k_type_tag_property[] = "__napi_type_tag__";
const char k_wrap_property[] = "__napi_wrap__";
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

bool napi_external__::is_external(JSValueConst value)
{
  return JS_GetClassID(value) == external_class_id;
}

void *napi_external__::get_value(JSValueConst value)
{
  if (!is_external(value))
    return nullptr;
  auto *hint = static_cast<napi_external_backing_store_hint__ *>(
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

void napi_external__::free_external_array_buffer_data(JSRuntime *rt, void *opaque, void *ptr)
{
  (void)ptr;
  auto *hint = reinterpret_cast<napi_external_backing_store_hint__ *>(opaque);
  if (hint == nullptr)
    return;
  hint->invoke_finalizer();
  if (hint->is_detaching())
    return;
  napi_external_backing_store_hint__::destroy_with_runtime(rt, hint);
}

void napi_external__::finalizer(JSRuntime *rt, JSValue value)
{
  auto *hint = static_cast<napi_external_backing_store_hint__ *>(JS_GetOpaque(value, external_class_id));
  if (hint == nullptr)
    return;

  hint->invoke_finalizer();
  napi_external_backing_store_hint__::destroy_with_runtime(rt, hint);
}
