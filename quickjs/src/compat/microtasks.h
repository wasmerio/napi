#ifndef NAPI_QUICKJS_COMPAT_MICROTASKS_H_
#define NAPI_QUICKJS_COMPAT_MICROTASKS_H_

#include "unofficial_napi.h"
#include <quickjs.h>

namespace quickjs::detail
{
    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv);
}

#endif // NAPI_QUICKJS_COMPAT_MICROTASKS_H_
