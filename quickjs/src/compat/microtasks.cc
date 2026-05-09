#include "compat/microtasks.h"

#include "compat/quickjs_utilities.h"

namespace quickjs::detail
{
    // Brief: QuickjsMicrotaskJob belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv)
    {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_UNDEFINED;
        return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
    }

}
