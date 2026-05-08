#ifndef NAPI_QUICKJS_COMPAT_MICROTASKS_H_
#define NAPI_QUICKJS_COMPAT_MICROTASKS_H_

#include "unofficial_napi.h"
#include <quickjs.h>

namespace quickjs::detail
{
    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv);
    void QuickjsPromiseHook(JSContext *ctx,
                            JSPromiseHookType type,
                            JSValueConst promise,
                            JSValueConst parent_promise,
                            void *opaque);
    void QuickjsPromiseRejectionTracker(JSContext *ctx,
                                        JSValueConst promise,
                                        JSValueConst reason,
                                        bool is_handled,
                                        void *opaque);
}

#endif // NAPI_QUICKJS_COMPAT_MICROTASKS_H_
