#ifndef NAPI_QUICKJS_COMPAT_CONTEXTIFY_H_
#define NAPI_QUICKJS_COMPAT_CONTEXTIFY_H_

#include "unofficial_napi.h"
#include <quickjs.h>

#include <cstdint>
#include <string>

namespace quickjs::detail
{
    void AnnotateContextifyCompileException(napi_env env,
                                            JSValueConst exception,
                                            const std::string &source,
                                            const std::string &resource_name,
                                            int32_t line_offset,
                                            int32_t column_offset);
}

#endif // NAPI_QUICKJS_COMPAT_CONTEXTIFY_H_
