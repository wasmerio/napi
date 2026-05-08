#include "compat/environment.h"

#include "compat/quickjs_utilities.h"
#include "internal/napi_env.h"

#include <chrono>

namespace quickjs::detail
{
    std::mutex g_mu;
    EmbedderHooksState g_embedder_hooks;
    std::unordered_map<napi_env, EnvState> g_env_states;

    // Brief: FreeStoredValue belongs to the environment state compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void FreeStoredValue(JSContext *ctx, JSValue *value)
    {
        if (value != nullptr && !JS_IsUndefined(*value))
        {
            JS_FreeValue(ctx, *value);
            *value = JS_UNDEFINED;
        }
    }

    // Brief: FreeEnvStateValues belongs to the environment state compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void FreeEnvStateValues(napi_env env, EnvState *state)
    {
        if (!CheckEnv(env) || state == nullptr)
            return;
        JSContext *ctx = Ctx(env);
        auto free_value = [ctx](JSValue *value) {
            if (!JS_IsUndefined(*value))
            {
                JS_FreeValue(ctx, *value);
                *value = JS_UNDEFINED;
            }
        };

        free_value(&state->prepare_stack_trace_callback);
        free_value(&state->promise_reject_callback);
        for (JSValue &hook : state->promise_hooks)
            free_value(&hook);
        free_value(&state->continuation_preserved_embedder_data);
        for (auto &entry : state->promise_context_frames)
            free_value(&entry.second);
        state->promise_context_frames.clear();
        for (JSValue &frame : state->promise_context_frame_stack)
            free_value(&frame);
        state->promise_context_frame_stack.clear();
        free_value(&state->import_module_dynamically_callback);
        free_value(&state->initialize_import_meta_object_callback);
        free_value(&state->error_formatting.get_source_map_error_source);
        free_value(&state->error_formatting.preserved_source_line);
        free_value(&state->error_formatting.preserved_thrown_at);
    }

    // Brief: EnsureEnvState belongs to the environment state compatibility layer.
    // It creates or returns the per-env side table used by unofficial N-API shims.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // The first access also initializes a non-zero hash seed for metadata APIs.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    EnvState &EnsureEnvState(napi_env env)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto &state = g_env_states[env];
        if (state.hash_seed == 1)
        {
            auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            state.hash_seed = static_cast<uint64_t>(ticks);
            if (state.hash_seed == 0)
                state.hash_seed = 1;
        }
        return state;
    }

    // Brief: DestroyEnvInstance belongs to the environment state compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status DestroyEnvInstance(napi_env env)
    {
        if (env == nullptr)
            return napi_invalid_arg;

        EnvState state;
        bool had_state = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_env_states.find(env);
            if (it != g_env_states.end())
            {
                state = it->second;
                had_state = true;
                g_env_states.erase(it);
            }
        }

        if (had_state && state.cleanup_callback != nullptr)
            state.cleanup_callback(env, state.cleanup_callback_data);
        if (had_state)
            FreeEnvStateValues(env, &state);
        if (had_state && state.destroy_callback != nullptr)
            state.destroy_callback(env, state.destroy_callback_data);

        JSContext *ctx = env->context();
        JSRuntime *rt = JS_GetRuntime(ctx);
        JS_SetPromiseHook(rt, nullptr, nullptr);
        JS_SetHostPromiseRejectionTracker(rt, nullptr, nullptr);
        JS_SetContextOpaque(ctx, nullptr);
        delete env;

        return napi_ok;
    }

    // Brief: ReleaseEnvScope belongs to the environment state compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_status ReleaseEnvScope(void *scope_ptr)
    {
        if (scope_ptr == nullptr)
            return napi_invalid_arg;

        auto *scope = static_cast<UnofficialEnvScope *>(scope_ptr);
        napi_status status = napi_ok;
        if (scope->env != nullptr)
        {
            status = DestroyEnvInstance(scope->env);
            scope->env = nullptr;
        }
        if (scope->ctx != nullptr)
        {
            JS_FreeContext(scope->ctx);
            scope->ctx = nullptr;
        }
        if (scope->rt != nullptr)
        {
            // JS_FreeRuntime(scope->rt);
            scope->rt = nullptr;
        }
        delete scope;
        return status;
    }
}
