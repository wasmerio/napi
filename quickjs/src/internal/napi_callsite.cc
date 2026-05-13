#include "internal/napi_callsite.h"

#include "internal/napi_util.h"

namespace quickjs::detail
{

namespace
{
constexpr uint32_t kMaxQuickJSCallSites = 256;

napi_status wrap_stack_trace(napi_env env,
                             uint32_t frames,
                             uint32_t skip_frames,
                             napi_value *callsites_out)
{
  if (!napi_util__::check_env(env) || callsites_out == nullptr)
    return napi_invalid_arg;
  if (frames == 0 || frames > kMaxQuickJSCallSites)
    return napi_invalid_arg;

  JSContext *ctx = napi_util__::context(env);
  JSValue stack = JS_GetCurrentStackTrace(ctx,
                                          static_cast<int>(frames),
                                          static_cast<int>(skip_frames));
  if (JS_IsException(stack))
    return napi_util__::return_pending_if_caught(env, "Failed to capture stack trace");

  return napi_util__::wrap_owned(env, stack, callsites_out);
}

bool get_uint32_property(JSContext *ctx, JSValueConst object, const char *name, uint32_t *out)
{
  JSValue value = JS_GetPropertyStr(ctx, object, name);
  bool ok = false;
  if (!JS_IsException(value))
    ok = JS_ToUint32(ctx, out, value) == 0;
  JS_FreeValue(ctx, value);
  if (!ok && JS_HasException(ctx))
    JS_FreeValue(ctx, JS_GetException(ctx));
  return ok;
}

bool string_property_has_text(JSContext *ctx, JSValueConst object, const char *name, JSValue *out)
{
  JSValue value = JS_GetPropertyStr(ctx, object, name);
  const char *text = nullptr;
  bool ok = false;

  if (JS_IsException(value))
    return false;
  if (JS_IsString(value))
  {
    text = JS_ToCString(ctx, value);
    ok = text != nullptr && text[0] != '\0';
    JS_FreeCString(ctx, text);
  }

  if (ok)
  {
    *out = value;
    return true;
  }

  JS_FreeValue(ctx, value);
  return false;
}
} // namespace

napi_status napi_callsite__::get_call_sites(napi_env env, uint32_t frames, napi_value *callsites_out)
{
  return wrap_stack_trace(env, frames, 1, callsites_out);
}

napi_status napi_callsite__::get_current_stack_trace(napi_env env, uint32_t frames, napi_value *callsites_out)
{
  return wrap_stack_trace(env, frames, 0, callsites_out);
}

napi_status napi_callsite__::get_caller_location(napi_env env, napi_value *location_out)
{
  if (!napi_util__::check_env(env) || location_out == nullptr)
    return napi_invalid_arg;

  JSContext *ctx = napi_util__::context(env);
  JSValue stack = JS_GetCurrentStackTrace(ctx, 1, 1);
  if (JS_IsException(stack))
    return napi_util__::return_pending_if_caught(env, "Failed to capture caller location");

  JSValue frame = JS_GetPropertyUint32(ctx, stack, 0);
  JS_FreeValue(ctx, stack);
  if (JS_IsException(frame))
    return napi_util__::return_pending_if_caught(env, "Failed to get caller frame");
  if (JS_IsUndefined(frame))
  {
    JS_FreeValue(ctx, frame);
    *location_out = nullptr;
    return napi_ok;
  }

  JSValue file = JS_UNDEFINED;
  if (!string_property_has_text(ctx, frame, "scriptNameOrSourceURL", &file) &&
      !string_property_has_text(ctx, frame, "scriptName", &file))
  {
    JS_FreeValue(ctx, frame);
    *location_out = nullptr;
    return napi_ok;
  }

  uint32_t line = 0;
  uint32_t column = 0;
  if (!get_uint32_property(ctx, frame, "lineNumber", &line) ||
      !get_uint32_property(ctx, frame, "columnNumber", &column))
  {
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, frame);
    *location_out = nullptr;
    return napi_ok;
  }

  JSValue location = JS_NewArray(ctx);
  if (JS_IsException(location))
  {
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, frame);
    return napi_util__::return_pending_if_caught(env, "Failed to create caller location");
  }

  if (JS_DefinePropertyValueUint32(ctx, location, 0, JS_NewUint32(ctx, line), JS_PROP_C_W_E) < 0 ||
      JS_DefinePropertyValueUint32(ctx, location, 1, JS_NewUint32(ctx, column), JS_PROP_C_W_E) < 0 ||
      JS_DefinePropertyValueUint32(ctx, location, 2, file, JS_PROP_C_W_E) < 0)
  {
    JS_FreeValue(ctx, location);
    JS_FreeValue(ctx, frame);
    return napi_util__::return_pending_if_caught(env, "Failed to populate caller location");
  }

  JS_FreeValue(ctx, frame);
  return napi_util__::wrap_owned(env, location, location_out);
}

} // namespace quickjs::detail
