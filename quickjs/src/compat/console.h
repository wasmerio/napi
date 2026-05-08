#ifndef NAPI_QUICKJS_COMPAT_CONSOLE_H_
#define NAPI_QUICKJS_COMPAT_CONSOLE_H_

#include "node_api.h"

namespace quickjs::detail
{
    void RepairBootstrapConsoleBindings(napi_env env);
}

#endif // NAPI_QUICKJS_COMPAT_CONSOLE_H_
