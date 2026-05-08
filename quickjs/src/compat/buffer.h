#ifndef NAPI_QUICKJS_COMPAT_BUFFER_H_
#define NAPI_QUICKJS_COMPAT_BUFFER_H_

#include "js_native_api.h"
#include <quickjs.h>

namespace quickjs::detail
{
    void install_runtime_buffer_prototype(napi_env env, JSValueConst buffer);
}

#endif // NAPI_QUICKJS_COMPAT_BUFFER_H_
