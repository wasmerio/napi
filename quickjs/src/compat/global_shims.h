#ifndef NAPI_QUICKJS_COMPAT_GLOBAL_SHIMS_H_
#define NAPI_QUICKJS_COMPAT_GLOBAL_SHIMS_H_

#include <quickjs.h>

namespace quickjs::detail
{
    void EnsureQuickjsGlobalCompat(JSContext *ctx);
}

#endif // NAPI_QUICKJS_COMPAT_GLOBAL_SHIMS_H_
