#include "compat/quickjs_utilities.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace quickjs::detail
{
    // Brief: CheckEnv belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool CheckEnv(napi_env env)
    {
        return env != nullptr && env->context() != nullptr;
    }

    // Brief: Ctx belongs to the general utility compatibility layer.
    // It centralizes access to the QuickJS context stored in an N-API env.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Callers are expected to validate the env before using this shortcut.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSContext *Ctx(napi_env env)
    {
        return env->context();
    }

    // Brief: Rt belongs to the general utility compatibility layer.
    // It centralizes access to the QuickJS runtime behind an N-API env.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Callers are expected to validate the env before using this shortcut.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSRuntime *Rt(napi_env env)
    {
        return JS_GetRuntime(env->context());
    }

    // Brief: UndefinedValue belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value UndefinedValue(napi_env env)
    {
        napi_value out = nullptr;
        napi_get_undefined(env, &out);
        return out;
    }

    // Brief: StartsWith belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool StartsWith(const std::string &value, const char *prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    // Brief: clear_quickjs_exception belongs to the general utility compatibility layer.
    // It drains a pending QuickJS exception when a compatibility fallback can continue.
    // Inputs stay as QuickJS handles owned by the caller.
    // The exception object is freed immediately and no replacement error is thrown.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void clear_quickjs_exception(JSContext *ctx)
    {
        if (!JS_HasException(ctx))
            return;
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }

    // Brief: StripFileUrl belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string StripFileUrl(const std::string &value)
    {
        if (!StartsWith(value, "file://"))
            return value;
        std::string path = value.substr(7);
        std::string out;
        out.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == '%' && i + 2 < path.size())
            {
                char hex[3] = {path[i + 1], path[i + 2], '\0'};
                char *end = nullptr;
                long ch = std::strtol(hex, &end, 16);
                if (end != hex && *end == '\0')
                {
                    out.push_back(static_cast<char>(ch));
                    i += 2;
                    continue;
                }
            }
            out.push_back(path[i]);
        }
        return out;
    }

    // Brief: PathToFileUrl belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string PathToFileUrl(const std::string &path)
    {
        if (StartsWith(path, "file://"))
            return path;
        std::string out = "file://";
        for (char ch : path)
        {
            if (ch == ' ')
                out += "%20";
            else
                out.push_back(ch);
        }
        return out;
    }

    // Brief: ResolveSymlinkComponents belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::filesystem::path ResolveSymlinkComponents(const std::filesystem::path &path)
    {
        std::filesystem::path current;
        for (const std::filesystem::path &part : path.lexically_normal())
        {
            if (part == "." || part.empty())
                continue;
            if (part == "..")
            {
                current /= part;
                continue;
            }
            if (part == path.root_name() || part == path.root_directory())
            {
                current /= part;
                continue;
            }

            std::filesystem::path candidate = current.empty() ? part : current / part;
            std::error_code ec;
            if (std::filesystem::is_symlink(candidate, ec) && !ec)
            {
                std::filesystem::path target = std::filesystem::read_symlink(candidate, ec);
                if (!ec)
                {
                    current = target.is_absolute() ? target : (candidate.parent_path() / target);
                    current = current.lexically_normal();
                    continue;
                }
            }
            current = candidate;
        }
        return current.lexically_normal();
    }

    // Brief: ReadTextFile belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string ReadTextFile(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            const std::filesystem::path resolved = ResolveSymlinkComponents(path);
            if (resolved != path.lexically_normal())
            {
                in.clear();
                in.open(resolved);
            }
        }
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Brief: NormalizeResolvedPath belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::filesystem::path NormalizeResolvedPath(const std::filesystem::path &path)
    {
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
        if (!ec)
            return resolved.lexically_normal();
        resolved = ResolveSymlinkComponents(path);
        if (resolved != path.lexically_normal())
            return resolved.lexically_normal();
        resolved = std::filesystem::absolute(path, ec);
        if (!ec)
            return resolved.lexically_normal();
        return path.lexically_normal();
    }

    // Brief: IsRegularFileFollowingSymlinks belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsRegularFileFollowingSymlinks(const std::filesystem::path &candidate, std::filesystem::path *out)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec)
        {
            *out = NormalizeResolvedPath(candidate);
            return true;
        }
        const std::filesystem::path resolved = ResolveSymlinkComponents(candidate);
        if (resolved != candidate.lexically_normal())
        {
            ec.clear();
            if (std::filesystem::is_regular_file(resolved, ec) && !ec)
            {
                *out = NormalizeResolvedPath(resolved);
                return true;
            }
        }
        return false;
    }

    // Brief: IsDirectoryFollowingSymlinks belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsDirectoryFollowingSymlinks(const std::filesystem::path &candidate, std::filesystem::path *out)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec)
        {
            *out = NormalizeResolvedPath(candidate);
            return true;
        }
        const std::filesystem::path resolved = ResolveSymlinkComponents(candidate);
        if (resolved != candidate.lexically_normal())
        {
            ec.clear();
            if (std::filesystem::is_directory(resolved, ec) && !ec)
            {
                *out = NormalizeResolvedPath(resolved);
                return true;
            }
        }
        return false;
    }

    // Brief: TryResolveAsFile belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolveAsFile(const std::filesystem::path &candidate, std::filesystem::path *out)
    {
        if (IsRegularFileFollowingSymlinks(candidate, out))
            return true;
        static const char *const extensions[] = {".js", ".mjs", ".json"};
        for (const char *ext : extensions)
        {
            std::filesystem::path with_ext = candidate;
            with_ext += ext;
            if (IsRegularFileFollowingSymlinks(with_ext, out))
                return true;
        }
        std::filesystem::path directory;
        if (IsDirectoryFollowingSymlinks(candidate, &directory))
        {
            if (TryResolveAsFile(directory / "index", out))
                return true;
        }
        return false;
    }

    // Brief: DupCString belongs to the general utility compatibility layer.
    // It allocates QuickJS-owned C strings for runtime callbacks.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Allocation failure is reported with a null pointer for QuickJS to consume.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    char *DupCString(JSContext *ctx, const std::string &value)
    {
        char *out = static_cast<char *>(js_malloc(ctx, value.size() + 1));
        if (out == nullptr)
            return nullptr;
        std::memcpy(out, value.c_str(), value.size() + 1);
        return out;
    }

    // Brief: ToUtf8 belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string ToUtf8(napi_env env, napi_value value)
    {
        if (!CheckEnv(env) || value == nullptr)
            return {};
        const char *str = JS_ToCString(Ctx(env), value->get_inner());
        if (str == nullptr)
            return {};
        std::string out(str);
        JS_FreeCString(Ctx(env), str);
        return out;
    }

    // Brief: ToUtf8 belongs to the general utility compatibility layer.
    // It converts a QuickJS value into a C++ UTF-8 string for diagnostics and paths.
    // Inputs stay as QuickJS handles owned by the caller.
    // Conversion failure returns an empty string and leaves ownership unchanged.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string ToUtf8(JSContext *ctx, JSValueConst value)
    {
        if (ctx == nullptr)
            return {};
        const char *str = JS_ToCString(ctx, value);
        if (str == nullptr)
            return {};
        std::string out(str);
        JS_FreeCString(ctx, str);
        return out;
    }

    // Brief: SetStringProperty belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void SetStringProperty(JSContext *ctx, JSValueConst object, const char *name, const std::string &value)
    {
        JS_SetPropertyStr(ctx, object, name, JS_NewStringLen(ctx, value.c_str(), value.size()));
    }

    // Brief: IsTruthyProperty belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsTruthyProperty(napi_env env, napi_value object, const char *name)
    {
        JSValue prop = JS_GetPropertyStr(Ctx(env), object->get_inner(), name);
        if (JS_IsException(prop))
            return false;
        bool out = JS_ToBool(Ctx(env), prop);
        JS_FreeValue(Ctx(env), prop);
        return out;
    }

    // Brief: WrapOwned belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status WrapOwned(napi_env env, JSValue value, napi_value *result)
    {
        if (result == nullptr)
        {
            JS_FreeValue(Ctx(env), value);
            return napi_invalid_arg;
        }
        *result = env->current_scope()->wrap_value(value, true);
        return (*result == nullptr) ? napi_generic_failure : napi_ok;
    }

    // Brief: WrapDup belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status WrapDup(napi_env env, JSValueConst value, napi_value *result)
    {
        return WrapOwned(env, JS_DupValue(Ctx(env), value), result);
    }

    // Brief: CreateEmptyArray belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status CreateEmptyArray(napi_env env, napi_value *result)
    {
        return WrapOwned(env, JS_NewArray(Ctx(env)), result);
    }

    // Brief: CreateUndefined belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status CreateUndefined(napi_env env, napi_value *result)
    {
        return WrapOwned(env, JS_UNDEFINED, result);
    }

    // Brief: IsCallable belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsCallable(napi_env env, napi_value value)
    {
        return value != nullptr && JS_IsFunction(Ctx(env), value->get_inner());
    }

    // Brief: RunPendingJobs belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status RunPendingJobs(napi_env env)
    {
        JSContext *job_ctx = nullptr;
        for (;;)
        {
            int rc = JS_ExecutePendingJob(Rt(env), &job_ctx);
            if (rc == 0)
                return napi_ok;
            if (rc < 0)
                return napi_pending_exception;
        }
    }

    // Brief: GetConstructorNameValue belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue GetConstructorNameValue(napi_env env, JSValueConst value)
    {
        JSContext *ctx = Ctx(env);
        JSValue ctor = JS_GetPropertyStr(ctx, value, "constructor");
        if (JS_IsException(ctor))
            return JS_EXCEPTION;
        JSValue name = JS_UNDEFINED;
        if (JS_IsObject(ctor))
            name = JS_GetPropertyStr(ctx, ctor, "name");
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(name))
            return JS_EXCEPTION;
        if (JS_IsUndefined(name))
            name = JS_NewString(ctx, "");
        return name;
    }

    // Brief: UnsupportedIfValidEnv belongs to the general utility compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status UnsupportedIfValidEnv(napi_env env)
    {
        return CheckEnv(env) ? napi_generic_failure : napi_invalid_arg;
    }
}
