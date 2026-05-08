#include "compat/buffer.h"

#include "compat/quickjs_utilities.h"
#include "internal/napi_env.h"

namespace quickjs::detail
{
    // Brief: install_runtime_buffer_prototype belongs to the Buffer prototype compatibility layer.
    // It links QuickJS-created Uint8Array buffers to the runtime Buffer prototype.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failure is deliberately non-fatal because raw typed arrays remain usable.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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
}
