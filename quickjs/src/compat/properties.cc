#include "compat/properties.h"

#include "compat/quickjs_utilities.h"

#include <cstring>

namespace quickjs::detail
{
    // Brief: exception_message_contains belongs to the property assignment compatibility layer.
    // It inspects QuickJS exception text without letting secondary conversion errors leak.
    // Inputs stay as QuickJS handles owned by the caller.
    // Failure to stringify is treated as a non-match after clearing nested exceptions.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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

    // Brief: should_define_own_property_after_set_failure belongs to the property assignment compatibility layer.
    // It recognizes the QuickJS inherited-readonly assignment shape Node expects to shadow.
    // Inputs stay as QuickJS handles owned by the caller.
    // Own-property lookup failures are cleared and reported as a conservative false.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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

    // Brief: set_property_with_node_compat belongs to the property assignment compatibility layer.
    // It first uses QuickJS normal property assignment semantics.
    // When QuickJS rejects inherited readonly properties, it falls back to a Node-like own property.
    // Inputs stay as QuickJS handles owned by the caller and values are duplicated before storing.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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
