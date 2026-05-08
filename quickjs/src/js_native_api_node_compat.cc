#include "js_native_api_node_compat.h"
#include "internal/napi_env.h"
#include <cstring>

namespace quickjs::detail
{
  void clear_quickjs_exception(JSContext *ctx)
  {
    if (!JS_HasException(ctx))
      return;
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
  }

  void install_runtime_buffer_prototype(napi_env env, JSValueConst buffer)
  {
    JSContext *ctx = env->context();
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global))
    {
      clear_quickjs_exception(ctx);
      return;
    }

    JSValue buffer_ctor = JS_GetPropertyStr(ctx, global, "Buffer");
    JS_FreeValue(ctx, global);
    if (JS_IsException(buffer_ctor))
    {
      clear_quickjs_exception(ctx);
      return;
    }
    if (!JS_IsObject(buffer_ctor))
    {
      JS_FreeValue(ctx, buffer_ctor);
      return;
    }

    JSValue prototype = JS_GetPropertyStr(ctx, buffer_ctor, "prototype");
    JS_FreeValue(ctx, buffer_ctor);
    if (JS_IsException(prototype))
    {
      clear_quickjs_exception(ctx);
      return;
    }

    if (JS_IsObject(prototype) && JS_SetPrototype(ctx, buffer, prototype) < 0)
    {
      clear_quickjs_exception(ctx);
    }
    JS_FreeValue(ctx, prototype);
  }

  bool exception_message_contains(JSContext *ctx, JSValueConst exception, const char *needle)
  {
    if (needle == nullptr)
      return false;

    JSValue message = JS_GetPropertyStr(ctx, exception, "message");
    const char *text = nullptr;
    if (!JS_IsException(message) && !JS_IsUndefined(message) && !JS_IsNull(message))
      text = JS_ToCString(ctx, message);
    else if (JS_IsException(message))
      clear_quickjs_exception(ctx);
    if (text == nullptr)
    {
      text = JS_ToCString(ctx, exception);
      if (text == nullptr && JS_HasException(ctx))
        clear_quickjs_exception(ctx);
    }

    bool found = text != nullptr && std::strstr(text, needle) != nullptr;
    if (text != nullptr)
      JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, message);
    return found;
  }

  bool should_define_own_property_after_set_failure(JSContext *ctx,
                                                    JSValueConst object,
                                                    JSAtom property,
                                                    JSValueConst exception)
  {
    if (!exception_message_contains(ctx, exception, "read-only") &&
        !exception_message_contains(ctx, exception, "no setter for property"))
    {
      return false;
    }

    int has_own = JS_GetOwnProperty(ctx, nullptr, object, property);
    if (has_own < 0)
    {
      clear_quickjs_exception(ctx);
      return false;
    }
    return has_own == 0;
  }

  int set_property_with_node_compat(JSContext *ctx,
                                    JSValueConst object,
                                    JSAtom property,
                                    JSValueConst value)
  {
    int rc = JS_SetProperty(ctx, object, property, JS_DupValue(ctx, value));
    if (rc >= 0 || !JS_HasException(ctx))
      return rc;

    JSValue exception = JS_GetException(ctx);
    if (should_define_own_property_after_set_failure(ctx, object, property, exception))
    {
      JS_FreeValue(ctx, exception);
      return JS_DefinePropertyValue(ctx,
                                    object,
                                    property,
                                    JS_DupValue(ctx, value),
                                    JS_PROP_C_W_E);
    }

    JS_Throw(ctx, exception);
    return -1;
  }
}
