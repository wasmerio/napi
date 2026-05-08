#ifndef NAPI_QUICKJS_COMPAT_MODULE_LOADING_H_
#define NAPI_QUICKJS_COMPAT_MODULE_LOADING_H_

#include "unofficial_napi.h"
#include <quickjs.h>

#include <cstdint>
#include <string>

namespace quickjs::detail
{
    enum QuickjsModuleWrapStatus : int32_t
    {
        kQuickjsModuleUninstantiated = 0,
        kQuickjsModuleInstantiating = 1,
        kQuickjsModuleInstantiated = 2,
        kQuickjsModuleEvaluating = 3,
        kQuickjsModuleEvaluated = 4,
        kQuickjsModuleErrored = 5,
    };

    struct QuickjsModuleWrap
    {
        JSValue module = JS_UNDEFINED;
        JSValue namespace_value = JS_UNDEFINED;
        JSValue error = JS_UNDEFINED;
        QuickjsModuleWrapStatus status = kQuickjsModuleUninstantiated;
        bool has_top_level_await = false;
    };

    char *QuickjsModuleNormalize(JSContext *ctx, const char *module_base_name, const char *module_name, void *opaque);
    JSModuleDef *QuickjsModuleLoader(JSContext *ctx, const char *module_name, void *opaque);
    JSModuleDef *ModuleDefFromValue(JSValueConst value);
    int SetModuleImportMetaUrl(JSContext *ctx, JSValueConst module_value, const std::string &url);
    void StoreModuleError(napi_env env, QuickjsModuleWrap *module);
}

#endif // NAPI_QUICKJS_COMPAT_MODULE_LOADING_H_
