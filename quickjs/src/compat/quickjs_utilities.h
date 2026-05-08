#ifndef NAPI_QUICKJS_COMPAT_QUICKJS_UTILITIES_H_
#define NAPI_QUICKJS_COMPAT_QUICKJS_UTILITIES_H_

#include "unofficial_napi.h"

#include <quickjs.h>

#include <filesystem>
#include <string>

namespace quickjs::detail
{
    bool CheckEnv(napi_env env);
    JSContext *Ctx(napi_env env);
    JSRuntime *Rt(napi_env env);
    napi_value UndefinedValue(napi_env env);
    bool StartsWith(const std::string &value, const char *prefix);
    void clear_quickjs_exception(JSContext *ctx);
    std::string StripFileUrl(const std::string &value);
    std::string PathToFileUrl(const std::string &path);
    std::filesystem::path ResolveSymlinkComponents(const std::filesystem::path &path);
    std::string ReadTextFile(const std::filesystem::path &path);
    std::filesystem::path NormalizeResolvedPath(const std::filesystem::path &path);
    bool IsRegularFileFollowingSymlinks(const std::filesystem::path &candidate, std::filesystem::path *out);
    bool IsDirectoryFollowingSymlinks(const std::filesystem::path &candidate, std::filesystem::path *out);
    bool TryResolveAsFile(const std::filesystem::path &candidate, std::filesystem::path *out);
    char *DupCString(JSContext *ctx, const std::string &value);
    std::string ToUtf8(napi_env env, napi_value value);
    std::string ToUtf8(JSContext *ctx, JSValueConst value);
    void SetStringProperty(JSContext *ctx, JSValueConst object, const char *name, const std::string &value);
    bool IsTruthyProperty(napi_env env, napi_value object, const char *name);
    napi_status WrapOwned(napi_env env, JSValue value, napi_value *result);
    napi_status WrapDup(napi_env env, JSValueConst value, napi_value *result);
    napi_status CreateEmptyArray(napi_env env, napi_value *result);
    napi_status CreateUndefined(napi_env env, napi_value *result);
    bool IsCallable(napi_env env, napi_value value);
    napi_status StoreOptionalFunction(napi_env env, napi_value callback, JSValue *target);
    napi_status RunPendingJobs(napi_env env);
    JSValue GetConstructorNameValue(napi_env env, JSValueConst value);
    napi_status UnsupportedIfValidEnv(napi_env env);
}

#endif // NAPI_QUICKJS_COMPAT_QUICKJS_UTILITIES_H_
