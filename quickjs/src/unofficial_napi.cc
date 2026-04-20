#include "internal/quickjs_env.h"

#include <unordered_map>
#include <mutex>

namespace
{
    std::mutex g_env_by_context_mu;
    std::unordered_map<JSContext *, napi_env> g_env_by_context;
}

struct UnofficialEnvScope
{
    JSRuntime *rt;
    JSContext *ctx;
    napi_env env;
};

napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
    JSContext *context, int32_t module_api_version, napi_env *result)
{
    if (result == nullptr || context == nullptr)
        return napi_invalid_arg;

    // TODO: find out if we need to set that somehow
    // context->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);

    auto *env = new (std::nothrow) napi_env__(context, module_api_version);
    if (env == nullptr)
        return napi_generic_failure;

    {
        std::lock_guard<std::mutex> lock{g_env_by_context_mu};
        g_env_by_context[env->ctx] = env;
    }

    *result = env;
    return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_create_env(int32_t module_api_version,
                                                  napi_env *env_out,
                                                  void **scope_out)
{
    // TODO: Find out who will dispose those and when
    // Check if with QuickJS we just create new JSRuntime for each "isolate".
    // We can't treat JSContext as "isolate", because then it won't be
    // thread-safe, or we'd need to use mutex when accessing JSRuntime, or
    // wrap access to JSRuntime with syntetic "Isolate" class and use mutex there.
    // Probably, for best performance, better to just have new JSRuntime for each "isolate".
    auto rt = JS_NewRuntime();
    if (!rt)
        return napi_generic_failure;

    auto ctx = JS_NewContext(rt);

    if (!ctx)
    {
        JS_FreeRuntime(rt);
        return napi_generic_failure;
    }

    // TODO: Someone needs to delete this later
    auto scope = new (std::nothrow) UnofficialEnvScope{.rt = rt, .ctx = ctx};

    auto status = unofficial_napi_create_env_from_context(ctx, module_api_version, &scope->env);
    if (status != napi_ok || scope->env == nullptr)
    {
        delete scope;
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return (status == napi_ok) ? napi_generic_failure : status;
    }

    *scope_out = reinterpret_cast<void *>(scope);
    *env_out = scope->env;

    return napi_ok;
}
