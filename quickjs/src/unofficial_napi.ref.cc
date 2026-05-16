#include "unofficial_napi.h"

#include "internal/napi_env.h"
#include "internal/napi_external.h"
#include "internal/napi_util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

struct UnofficialEnvScope
{
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    napi_env env = nullptr;
};

namespace
{
    struct ErrorFormattingState
    {
        bool source_maps_enabled = false;
        JSValue get_source_map_error_source = JS_UNDEFINED;
        JSValue preserved_source_line = JS_UNDEFINED;
        JSValue preserved_thrown_at = JS_UNDEFINED;
    };

    struct EnvState
    {
        void *edge_environment = nullptr;
        unofficial_napi_env_cleanup_callback cleanup_callback = nullptr;
        void *cleanup_callback_data = nullptr;
        unofficial_napi_env_destroy_callback destroy_callback = nullptr;
        void *destroy_callback_data = nullptr;
        unofficial_napi_context_token_callback context_token_assign_callback = nullptr;
        unofficial_napi_context_token_callback context_token_unassign_callback = nullptr;
        void *context_token_callback_data = nullptr;
        unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback = nullptr;
        void *enqueue_foreground_task_target = nullptr;
        unofficial_napi_fatal_error_callback fatal_error_callback = nullptr;
        unofficial_napi_oom_error_callback oom_error_callback = nullptr;
        unofficial_napi_near_heap_limit_callback near_heap_limit_callback = nullptr;
        void *near_heap_limit_callback_data = nullptr;
        void *stack_limit = nullptr;
        JSValue prepare_stack_trace_callback = JS_UNDEFINED;
        JSValue promise_reject_callback = JS_UNDEFINED;
        JSValue promise_hooks[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
        JSValue continuation_preserved_embedder_data = JS_UNDEFINED;
        std::unordered_map<void *, JSValue> promise_context_frames;
        std::vector<JSValue> promise_context_frame_stack;
        JSValue import_module_dynamically_callback = JS_UNDEFINED;
        JSValue initialize_import_meta_object_callback = JS_UNDEFINED;
        ErrorFormattingState error_formatting;
        uint64_t hash_seed = 1;
    };

    struct EmbedderHooksState
    {
        unofficial_napi_embedder_hooks hooks{};
    };

    struct SerializedValue
    {
        size_t length = 0;
        uint8_t bytes[];
    };

    std::mutex g_mu;
    EmbedderHooksState g_embedder_hooks;
    std::unordered_map<napi_env, EnvState> g_env_states;

    EnvState &EnsureEnvState(napi_env env);

    bool CheckEnv(napi_env env)
    {
        return env != nullptr && env->context() != nullptr;
    }

    JSContext *Ctx(napi_env env)
    {
        return env->context();
    }

    JSRuntime *Rt(napi_env env)
    {
        return JS_GetRuntime(env->context());
    }

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

    bool ContextifyCompileTraceEnabled()
    {
        return std::getenv("EDGE_TRACE_QUICKJS_CONTEXTIFY") != nullptr ||
               std::getenv("EDGE_TRACE_BUILTINS") != nullptr;
    }

    int32_t GetInt32PropertyOr(JSContext *ctx, JSValueConst object, const char *name, int32_t fallback)
    {
        JSValue value = JS_GetPropertyStr(ctx, object, name);
        if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value))
        {
            JS_FreeValue(ctx, value);
            return fallback;
        }
        int32_t out = fallback;
        (void)JS_ToInt32(ctx, &out, value);
        JS_FreeValue(ctx, value);
        return out;
    }

    std::string GetStringPropertyOrEmpty(JSContext *ctx, JSValueConst object, const char *name)
    {
        JSValue value = JS_GetPropertyStr(ctx, object, name);
        if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value))
        {
            JS_FreeValue(ctx, value);
            return {};
        }
        std::string out = ToUtf8(ctx, value);
        JS_FreeValue(ctx, value);
        return out;
    }

    std::string BuiltinIdFromResourceName(const std::string &resource_name)
    {
        const char prefix[] = "node:";
        if (resource_name.rfind(prefix, 0) == 0)
            return resource_name.substr(sizeof(prefix) - 1);
        return {};
    }

    std::string SourceLineAt(const std::string &source, int32_t one_based_line)
    {
        if (source.empty() || one_based_line <= 0)
            return {};
        size_t pos = 0;
        for (int32_t line = 1; line < one_based_line; ++line)
        {
            pos = source.find('\n', pos);
            if (pos == std::string::npos)
                return {};
            ++pos;
        }
        size_t end = source.find('\n', pos);
        std::string line = source.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 240)
            line = line.substr(0, 240) + "...";
        return line;
    }

    void SetStringProperty(JSContext *ctx, JSValueConst object, const char *name, const std::string &value)
    {
        JS_SetPropertyStr(ctx, object, name, JS_NewStringLen(ctx, value.c_str(), value.size()));
    }

    void SetInt32Property(JSContext *ctx, JSValueConst object, const char *name, int32_t value)
    {
        JS_SetPropertyStr(ctx, object, name, JS_NewInt32(ctx, value));
    }

    void AnnotateContextifyCompileException(napi_env env,
                                            JSValueConst exception,
                                            const std::string &source,
                                            const std::string &resource_name,
                                            int32_t line_offset,
                                            int32_t column_offset)
    {
        if (!CheckEnv(env) || !JS_IsObject(exception))
            return;

        JSContext *ctx = Ctx(env);
        const std::string builtin_id = BuiltinIdFromResourceName(resource_name);
        const std::string quickjs_file = GetStringPropertyOrEmpty(ctx, exception, "fileName");
        const int32_t quickjs_line = GetInt32PropertyOr(ctx, exception, "lineNumber", -1);
        const int32_t mapped_line = quickjs_line > 0 ? quickjs_line + line_offset : -1;

        JS_SetPropertyStr(ctx, exception, "node:quickjsContextifyCompile", JS_NewBool(ctx, true));
        SetStringProperty(ctx, exception, "node:quickjsCompileResourceName", resource_name);
        if (!builtin_id.empty())
            SetStringProperty(ctx, exception, "node:quickjsCompileBuiltinId", builtin_id);
        SetInt32Property(ctx, exception, "node:quickjsCompileLineOffset", line_offset);
        SetInt32Property(ctx, exception, "node:quickjsCompileColumnOffset", column_offset);
        if (quickjs_line > 0)
            SetInt32Property(ctx, exception, "node:quickjsCompileQuickJSLine", quickjs_line);
        if (mapped_line > 0)
            SetInt32Property(ctx, exception, "node:quickjsCompileMappedLine", mapped_line);

        if (!ContextifyCompileTraceEnabled())
            return;

        std::string summary = "[quickjs contextify compile]";
        if (!resource_name.empty())
            summary += " resource=" + resource_name;
        if (!builtin_id.empty())
            summary += " builtin=" + builtin_id;
        if (!quickjs_file.empty())
            summary += " quickjsFile=" + quickjs_file;
        if (quickjs_line > 0)
            summary += " quickjsLine=" + std::to_string(quickjs_line);
        if (mapped_line > 0)
            summary += " mappedLine=" + std::to_string(mapped_line);
        summary += " lineOffset=" + std::to_string(line_offset);
        summary += " columnOffset=" + std::to_string(column_offset);

        std::string source_line = SourceLineAt(source, quickjs_line);
        if (!source_line.empty())
            summary += " sourceLine=\"" + source_line + "\"";

        std::fprintf(stderr, "%s\n", summary.c_str());

        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        std::string stack_text;
        if (!JS_IsException(stack) && !JS_IsUndefined(stack) && !JS_IsNull(stack))
            stack_text = ToUtf8(ctx, stack);
        JS_FreeValue(ctx, stack);
        if (!stack_text.empty())
            SetStringProperty(ctx, exception, "stack", summary + "\n" + stack_text);
    }

    bool IsTruthyProperty(napi_env env, napi_value object, const char *name)
    {
        JSValue prop = JS_GetPropertyStr(Ctx(env), object->get_inner(), name);
        if (JS_IsException(prop))
            return false;
        bool out = JS_ToBool(Ctx(env), prop);
        JS_FreeValue(Ctx(env), prop);
        return out;
    }

    void FreeStoredValue(JSContext *ctx, JSValue *value)
    {
        if (value != nullptr && !JS_IsUndefined(*value))
        {
            JS_FreeValue(ctx, *value);
            *value = JS_UNDEFINED;
        }
    }

    void ReplaceStoredValue(napi_env env, JSValue *target, JSValueConst value)
    {
        JSContext *ctx = Ctx(env);
        FreeStoredValue(ctx, target);
        *target = JS_DupValue(ctx, value);
    }

    void *JsIdentity(JSValueConst value)
    {
        return JS_IsObject(value) ? JS_VALUE_GET_PTR(value) : nullptr;
    }

    void ClearPendingExceptionIfAny(JSContext *ctx)
    {
        if (ctx == nullptr || !JS_HasException(ctx))
            return;
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
    }

    void CallPromiseHook(napi_env env, JSValueConst hook, int argc, JSValueConst *argv)
    {
        if (!CheckEnv(env) || !JS_IsFunction(Ctx(env), hook))
            return;

        JSContext *ctx = Ctx(env);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ret = JS_Call(ctx, hook, global, argc, argv);
        JS_FreeValue(ctx, global);
        if (JS_IsException(ret))
        {
            ClearPendingExceptionIfAny(ctx);
            return;
        }
        JS_FreeValue(ctx, ret);
    }

    void CapturePromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;
        void *identity = JsIdentity(promise);
        if (identity == nullptr)
            return;

        auto &state = EnsureEnvState(env);
        if (JS_IsUndefined(state.continuation_preserved_embedder_data))
            return;
        JSContext *ctx = Ctx(env);
        JSValue frame = JS_DupValue(ctx, state.continuation_preserved_embedder_data);
        auto it = state.promise_context_frames.find(identity);
        if (it != state.promise_context_frames.end())
        {
            FreeStoredValue(ctx, &it->second);
            it->second = frame;
        }
        else
        {
            state.promise_context_frames.emplace(identity, frame);
        }
    }

    void EnterPromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;

        auto &state = EnsureEnvState(env);
        JSContext *ctx = Ctx(env);
        state.promise_context_frame_stack.push_back(
            JS_DupValue(ctx, state.continuation_preserved_embedder_data));

        JSValueConst frame = JS_UNDEFINED;
        void *identity = JsIdentity(promise);
        if (identity != nullptr)
        {
            auto it = state.promise_context_frames.find(identity);
            if (it != state.promise_context_frames.end())
                frame = it->second;
        }
        ReplaceStoredValue(env, &state.continuation_preserved_embedder_data, frame);
    }

    void LeavePromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;

        auto &state = EnsureEnvState(env);
        JSContext *ctx = Ctx(env);
        if (!state.promise_context_frame_stack.empty())
        {
            JSValue previous = state.promise_context_frame_stack.back();
            state.promise_context_frame_stack.pop_back();
            FreeStoredValue(ctx, &state.continuation_preserved_embedder_data);
            state.continuation_preserved_embedder_data = previous;
        }

        void *identity = JsIdentity(promise);
        if (identity != nullptr)
        {
            auto it = state.promise_context_frames.find(identity);
            if (it != state.promise_context_frames.end())
            {
                FreeStoredValue(ctx, &it->second);
                state.promise_context_frames.erase(it);
            }
        }
    }

    JSValue GetStoredFunction(napi_env env, JSValueConst value)
    {
        if (!CheckEnv(env) || !JS_IsFunction(Ctx(env), value))
            return JS_UNDEFINED;
        return JS_DupValue(Ctx(env), value);
    }

    JSValue GetPromiseHook(napi_env env, size_t index)
    {
        if (!CheckEnv(env) || index >= 4)
            return JS_UNDEFINED;
        auto &state = EnsureEnvState(env);
        return GetStoredFunction(env, state.promise_hooks[index]);
    }

    JSValue GetPromiseRejectCallback(napi_env env)
    {
        if (!CheckEnv(env))
            return JS_UNDEFINED;
        auto &state = EnsureEnvState(env);
        return GetStoredFunction(env, state.promise_reject_callback);
    }

    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv)
    {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_UNDEFINED;
        return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
    }

    void QuickjsPromiseHook(JSContext *ctx,
                            JSPromiseHookType type,
                            JSValueConst promise,
                            JSValueConst parent_promise,
                            void *opaque)
    {
        napi_env env = static_cast<napi_env>(opaque);
        if (!CheckEnv(env) || Ctx(env) != ctx)
            return;

        if (type == JS_PROMISE_HOOK_INIT)
            CapturePromiseContextFrame(env, promise);
        else if (type == JS_PROMISE_HOOK_BEFORE)
            EnterPromiseContextFrame(env, promise);

        size_t hook_index = static_cast<size_t>(type);
        JSValue hook = GetPromiseHook(env, hook_index);
        if (JS_IsFunction(ctx, hook))
        {
            if (type == JS_PROMISE_HOOK_INIT)
            {
                JSValueConst argv[] = {promise, parent_promise};
                CallPromiseHook(env, hook, 2, argv);
            }
            else
            {
                JSValueConst argv[] = {promise};
                CallPromiseHook(env, hook, 1, argv);
            }
        }
        JS_FreeValue(ctx, hook);

        if (type == JS_PROMISE_HOOK_AFTER)
            LeavePromiseContextFrame(env, promise);
    }

    void QuickjsPromiseRejectionTracker(JSContext *ctx,
                                        JSValueConst promise,
                                        JSValueConst reason,
                                        bool is_handled,
                                        void *opaque)
    {
        napi_env env = static_cast<napi_env>(opaque);
        if (!CheckEnv(env) || Ctx(env) != ctx)
            return;

        JSValue callback = GetPromiseRejectCallback(env);
        if (!JS_IsFunction(ctx, callback))
        {
            JS_FreeValue(ctx, callback);
            return;
        }

        JSValue event_type = JS_NewInt32(ctx, is_handled ? 1 : 0);
        JSValueConst argv[] = {event_type, promise, reason};
        CallPromiseHook(env, callback, 3, argv);
        JS_FreeValue(ctx, event_type);
        JS_FreeValue(ctx, callback);
    }

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
            JS_FreeRuntime(scope->rt);
            scope->rt = nullptr;
        }
        delete scope;
        return status;
    }

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

    napi_status WrapDup(napi_env env, JSValueConst value, napi_value *result)
    {
        return WrapOwned(env, JS_DupValue(Ctx(env), value), result);
    }

    napi_status CreateEmptyArray(napi_env env, napi_value *result)
    {
        return WrapOwned(env, JS_NewArray(Ctx(env)), result);
    }

    napi_status CreateUndefined(napi_env env, napi_value *result)
    {
        return WrapOwned(env, JS_UNDEFINED, result);
    }

    bool IsCallable(napi_env env, napi_value value)
    {
        return value != nullptr && JS_IsFunction(Ctx(env), value->get_inner());
    }

    napi_status StoreOptionalFunction(napi_env env, napi_value callback, JSValue *target)
    {
        if (target == nullptr)
            return napi_invalid_arg;
        if (callback != nullptr && !JS_IsUndefined(callback->get_inner()) && !JS_IsNull(callback->get_inner()) &&
            !IsCallable(env, callback))
            return napi_function_expected;

        if (!JS_IsUndefined(*target))
            JS_FreeValue(Ctx(env), *target);
        *target = (callback == nullptr) ? JS_UNDEFINED : JS_DupValue(Ctx(env), callback->get_inner());
        return napi_ok;
    }

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

    napi_status UnsupportedIfValidEnv(napi_env env)
    {
        return CheckEnv(env) ? napi_generic_failure : napi_invalid_arg;
    }

    void EnsureSymbolProperty(JSContext *ctx,
                              JSValueConst symbol_ctor,
                              const char *name,
                              const char *description)
    {
        JSValue existing = JS_GetPropertyStr(ctx, symbol_ctor, name);
        if (JS_IsException(existing))
            return;
        bool missing = JS_IsUndefined(existing);
        JS_FreeValue(ctx, existing);
        if (!missing)
            return;

        JS_DefinePropertyValueStr(
            ctx, symbol_ctor, name, JS_NewSymbol(ctx, description, false), 0);
    }

    void EnsureNodeWellKnownSymbols(JSContext *ctx)
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
        JS_FreeValue(ctx, global);
        if (JS_IsException(symbol_ctor) || !JS_IsObject(symbol_ctor))
        {
            JS_FreeValue(ctx, symbol_ctor);
            return;
        }

        EnsureSymbolProperty(ctx, symbol_ctor, "dispose", "Symbol.dispose");
        EnsureSymbolProperty(ctx, symbol_ctor, "asyncDispose", "Symbol.asyncDispose");
        JS_FreeValue(ctx, symbol_ctor);
    }
}

extern "C"
{

    napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
        JSContext *context, int32_t module_api_version, napi_env *result)
    {
        if (result == nullptr || context == nullptr)
            return napi_invalid_arg;

        auto rt = JS_GetRuntime(context);
        if (0 != napi_external__::register_class(rt))
            return napi_generic_failure;

        auto env = new (std::nothrow) napi_env__(context, module_api_version);
        if (env == nullptr)
            return napi_generic_failure;
        if (env->root_scope() == nullptr)
        {
            delete env;
            return napi_generic_failure;
        }

        JS_SetContextOpaque(context, env);
        EnsureEnvState(env);
        JS_SetPromiseHook(rt, QuickjsPromiseHook, env);

        *result = env;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_create_env(int32_t module_api_version,
                                                      napi_env *env_out,
                                                      void **scope_out)
    {
        return unofficial_napi_create_env_with_options(module_api_version, nullptr, env_out, scope_out);
    }

    napi_status NAPI_CDECL unofficial_napi_create_env_with_options(
        int32_t module_api_version,
        const unofficial_napi_env_create_options *options,
        napi_env *env_out,
        void **scope_out)
    {
        if (env_out == nullptr || scope_out == nullptr)
            return napi_invalid_arg;

        auto rt = JS_NewRuntime();
        if (rt == nullptr)
            return napi_generic_failure;
        if (options != nullptr)
        {
            if (options->max_old_generation_size_in_bytes > 0)
                JS_SetMemoryLimit(rt, options->max_old_generation_size_in_bytes);
        }

        auto ctx = JS_NewContext(rt);
        if (ctx == nullptr)
        {
            JS_FreeRuntime(rt);
            return napi_generic_failure;
        }
        EnsureNodeWellKnownSymbols(ctx);

        auto scope = new (std::nothrow) UnofficialEnvScope{.rt = rt, .ctx = ctx};
        if (scope == nullptr)
        {
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return napi_generic_failure;
        }

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

    napi_status NAPI_CDECL unofficial_napi_set_embedder_hooks(
        const unofficial_napi_embedder_hooks *hooks)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_embedder_hooks.hooks = (hooks == nullptr) ? unofficial_napi_embedder_hooks{} : *hooks;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_edge_environment(napi_env env, void *environment)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        EnsureEnvState(env).edge_environment = environment;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_env_cleanup_callback(
        napi_env env,
        unofficial_napi_env_cleanup_callback callback,
        void *data)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.cleanup_callback = callback;
        state.cleanup_callback_data = data;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_env_destroy_callback(
        napi_env env,
        unofficial_napi_env_destroy_callback callback,
        void *data)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.destroy_callback = callback;
        state.destroy_callback_data = data;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_context_token_callbacks(
        napi_env env,
        unofficial_napi_context_token_callback assign_callback,
        unofficial_napi_context_token_callback unassign_callback,
        void *data)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.context_token_assign_callback = assign_callback;
        state.context_token_unassign_callback = unassign_callback;
        state.context_token_callback_data = data;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_release_env(void *scope)
    {
        return ReleaseEnvScope(scope);
    }

    napi_status NAPI_CDECL unofficial_napi_release_env_with_loop(
        void *scope,
        struct uv_loop_s *loop)
    {
        (void)loop;
        return ReleaseEnvScope(scope);
    }

    napi_status NAPI_CDECL unofficial_napi_low_memory_notification(napi_env env)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        JS_RunGC(Rt(env));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_flags_from_string(
        const char *flags,
        size_t length)
    {
        (void)flags;
        (void)length;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_prepare_stack_trace_callback(
        napi_env env,
        napi_value callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return StoreOptionalFunction(env, callback, &EnsureEnvState(env).prepare_stack_trace_callback);
    }

    napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env)
    {
        return unofficial_napi_low_memory_notification(env);
    }

    napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return RunPendingJobs(env);
    }

    napi_status NAPI_CDECL unofficial_napi_terminate_execution(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_cancel_terminate_execution(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_request_interrupt(
        napi_env env,
        unofficial_napi_interrupt_callback callback,
        void *data)
    {
        if (!CheckEnv(env) || callback == nullptr)
            return napi_invalid_arg;
        callback(env, data);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_enqueue_foreground_task_callback(
        napi_env env,
        unofficial_napi_enqueue_foreground_task_callback callback,
        void *target)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.enqueue_foreground_task_callback = callback;
        state.enqueue_foreground_task_target = target;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback)
    {
        if (!CheckEnv(env) || !IsCallable(env, callback))
            return napi_invalid_arg;
        JSContext *ctx = Ctx(env);
        JSValueConst argv[] = {callback->get_inner()};
        if (JS_EnqueueJob(ctx, QuickjsMicrotaskJob, 1, argv) < 0)
            return JS_HasException(ctx) ? napi_pending_exception : napi_generic_failure;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                       napi_value callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        napi_status status = StoreOptionalFunction(env, callback, &state.promise_reject_callback);
        if (status != napi_ok)
            return status;
        JS_SetHostPromiseRejectionTracker(
            Rt(env),
            JS_IsFunction(Ctx(env), state.promise_reject_callback) ? QuickjsPromiseRejectionTracker : nullptr,
            env);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_hooks(napi_env env,
                                                             napi_value init,
                                                             napi_value before,
                                                             napi_value after,
                                                             napi_value resolve)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        napi_value callbacks[] = {init, before, after, resolve};
        for (size_t i = 0; i < 4; ++i)
        {
            napi_status status = StoreOptionalFunction(env, callbacks[i], &state.promise_hooks[i]);
            if (status != napi_ok)
                return status;
        }
        JS_SetPromiseHook(Rt(env), QuickjsPromiseHook, env);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_fatal_error_callbacks(
        napi_env env,
        unofficial_napi_fatal_error_callback fatal_callback,
        unofficial_napi_oom_error_callback oom_callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.fatal_error_callback = fatal_callback;
        state.oom_error_callback = oom_callback;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_near_heap_limit_callback(
        napi_env env,
        unofficial_napi_near_heap_limit_callback callback,
        void *data)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.near_heap_limit_callback = callback;
        state.near_heap_limit_callback_data = data;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_remove_near_heap_limit_callback(
        napi_env env,
        size_t heap_limit)
    {
        (void)heap_limit;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        state.near_heap_limit_callback = nullptr;
        state.near_heap_limit_callback_data = nullptr;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_stack_limit(napi_env env, void *stack_limit)
    {
        if (!CheckEnv(env) || stack_limit == nullptr)
            return napi_invalid_arg;
        EnsureEnvState(env).stack_limit = stack_limit;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_promise_details(napi_env env,
                                                               napi_value promise,
                                                               int32_t *state_out,
                                                               napi_value *result_out,
                                                               bool *has_result_out)
    {
        if (!CheckEnv(env) || promise == nullptr || state_out == nullptr || has_result_out == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = Ctx(env);
        JSValue result = JS_GetPropertyStr(ctx, promise->get_inner(), "result");
        if (JS_IsException(result))
            return napi_pending_exception;
        *state_out = 0;
        *has_result_out = !JS_IsUndefined(result);
        if (result_out != nullptr)
            return WrapOwned(env, result, result_out);
        JS_FreeValue(ctx, result);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_positions(
        napi_env env,
        napi_value error,
        unofficial_napi_error_source_positions *out)
    {
        if (!CheckEnv(env) || error == nullptr || out == nullptr)
            return napi_invalid_arg;
        std::memset(out, 0, sizeof(*out));
        out->line_number = -1;
        out->start_column = -1;
        out->end_column = -1;
        napi_value empty = nullptr;
        napi_status status = napi_create_string_utf8(env, "", 0, &empty);
        if (status != napi_ok)
            return status;
        out->source_line = empty;
        out->script_resource_name = empty;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(
        napi_env env,
        napi_value error)
    {
        if (!CheckEnv(env) || error == nullptr)
            return napi_invalid_arg;

        JSValue callback = JS_UNDEFINED;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_env_states.find(env);
            if (it == g_env_states.end() ||
                !it->second.error_formatting.source_maps_enabled ||
                JS_IsUndefined(it->second.error_formatting.get_source_map_error_source))
            {
                return napi_ok;
            }
            callback = JS_DupValue(Ctx(env), it->second.error_formatting.get_source_map_error_source);
        }

        JSValue mapped = JS_Call(Ctx(env), callback, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(Ctx(env), callback);
        if (JS_IsException(mapped))
        {
            JSValue exc = JS_GetException(Ctx(env));
            JS_FreeValue(Ctx(env), exc);
            return napi_generic_failure;
        }

        if (JS_IsString(mapped))
            JS_SetPropertyStr(Ctx(env), error->get_inner(), "node:arrowMessage", mapped);
        else
            JS_FreeValue(Ctx(env), mapped);

        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(
        napi_env env,
        bool enabled)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        EnsureEnvState(env).error_formatting.source_maps_enabled = enabled;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(
        napi_env env,
        napi_value callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return StoreOptionalFunction(env, callback, &EnsureEnvState(env).error_formatting.get_source_map_error_source);
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_line_for_stderr(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || error == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue value = JS_GetPropertyStr(Ctx(env), error->get_inner(), "node:arrowMessage");
        if (JS_IsException(value))
            return napi_pending_exception;
        if (JS_IsUndefined(value))
        {
            JS_FreeValue(Ctx(env), value);
            return CreateUndefined(env, result_out);
        }
        return WrapOwned(env, value, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_thrown_at(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        (void)error;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_take_preserved_error_formatting(
        napi_env env,
        napi_value error,
        napi_value *source_line_out,
        napi_value *thrown_at_out)
    {
        if (!CheckEnv(env) || error == nullptr || source_line_out == nullptr || thrown_at_out == nullptr)
            return napi_invalid_arg;
        napi_status status = unofficial_napi_get_error_source_line_for_stderr(env, error, source_line_out);
        if (status != napi_ok)
            return status;
        return CreateUndefined(env, thrown_at_out);
    }

    napi_status NAPI_CDECL unofficial_napi_mark_promise_as_handled(
        napi_env env,
        napi_value promise)
    {
        return (!CheckEnv(env) || promise == nullptr) ? napi_invalid_arg : napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_proxy_details(napi_env env,
                                                             napi_value proxy,
                                                             napi_value *target_out,
                                                             napi_value *handler_out)
    {
        (void)proxy;
        if (!CheckEnv(env) || target_out == nullptr || handler_out == nullptr)
            return napi_invalid_arg;
        *target_out = nullptr;
        *handler_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_preview_entries(napi_env env,
                                                           napi_value value,
                                                           napi_value *entries_out,
                                                           bool *is_key_value_out)
    {
        if (!CheckEnv(env) || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr)
            return napi_invalid_arg;
        *is_key_value_out = true;
        return CreateEmptyArray(env, entries_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_call_sites(napi_env env,
                                                          uint32_t frames,
                                                          napi_value *callsites_out)
    {
        (void)frames;
        if (!CheckEnv(env) || callsites_out == nullptr)
            return napi_invalid_arg;
        return CreateEmptyArray(env, callsites_out);
    }

    napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                       napi_value value,
                                                                       bool *result_out)
    {
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue buffer = JS_GetTypedArrayBuffer(Ctx(env), value->get_inner(), nullptr, nullptr, nullptr);
        if (JS_IsException(buffer))
        {
            JSValue exc = JS_GetException(Ctx(env));
            JS_FreeValue(Ctx(env), exc);
            *result_out = false;
            return napi_ok;
        }
        *result_out = !JS_IsUndefined(buffer) && !JS_IsNull(buffer);
        JS_FreeValue(Ctx(env), buffer);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_constructor_name(napi_env env,
                                                                napi_value value,
                                                                napi_value *name_out)
    {
        if (!CheckEnv(env) || value == nullptr || name_out == nullptr)
            return napi_invalid_arg;
        JSValue name = GetConstructorNameValue(env, value->get_inner());
        if (JS_IsException(name))
            return napi_pending_exception;
        return WrapOwned(env, name, name_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_own_non_index_properties(
        napi_env env,
        napi_value value,
        uint32_t filter_bits,
        napi_value *result_out)
    {
        (void)filter_bits;
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::get_property_names(env,
                                               value,
                                               napi_key_own_only,
                                               napi_key_all_properties,
                                               napi_key_keep_numbers,
                                               result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_create_private_symbol(napi_env env,
                                                                 const char *utf8description,
                                                                 size_t length,
                                                                 napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        const size_t description_length =
            utf8description == nullptr ? 0
            : length == NAPI_AUTO_LENGTH ? std::strlen(utf8description)
                                         : length;
        std::string description(utf8description == nullptr ? "" : utf8description,
                                description_length);
        JSValue symbol = JS_NewSymbol(Ctx(env), description.c_str(), false);
        if (JS_IsException(symbol))
            return napi_pending_exception;
        return WrapOwned(env, symbol, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_structured_clone(
        napi_env env,
        napi_value value,
        napi_value transfer_list_or_null,
        napi_value *result_out)
    {
        (void)transfer_list_or_null;
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
            return napi_generic_failure;
        JSValue cloned = JS_ReadObject(Ctx(env),
                                       bytes,
                                       size,
                                       JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        js_free(Ctx(env), bytes);
        if (JS_IsException(cloned))
            return napi_pending_exception;
        return WrapOwned(env, cloned, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_serialize_value(
        napi_env env,
        napi_value value,
        void **payload_out)
    {
        if (!CheckEnv(env) || value == nullptr || payload_out == nullptr)
            return napi_invalid_arg;
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
            return napi_generic_failure;
        auto *payload = static_cast<SerializedValue *>(std::malloc(sizeof(SerializedValue) + size));
        if (payload == nullptr)
        {
            js_free(Ctx(env), bytes);
            return napi_generic_failure;
        }
        payload->length = size;
        if (size > 0)
            std::memcpy(payload->bytes, bytes, size);
        js_free(Ctx(env), bytes);
        *payload_out = payload;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_deserialize_value(
        napi_env env,
        void *payload,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || payload == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        auto *serialized = static_cast<SerializedValue *>(payload);
        JSValue value = JS_ReadObject(Ctx(env),
                                      serialized->bytes,
                                      serialized->length,
                                      JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        if (JS_IsException(value))
            return napi_pending_exception;
        return WrapOwned(env, value, result_out);
    }

    void NAPI_CDECL unofficial_napi_release_serialized_value(void *payload)
    {
        std::free(payload);
    }

    napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
        napi_env env,
        double *heap_total_out,
        double *heap_used_out,
        double *external_out,
        double *array_buffers_out)
    {
        if (!CheckEnv(env) || heap_total_out == nullptr || heap_used_out == nullptr ||
            external_out == nullptr || array_buffers_out == nullptr)
            return napi_invalid_arg;

        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        *heap_total_out = static_cast<double>(usage.malloc_size);
        *heap_used_out = static_cast<double>(usage.memory_used_size);
        *external_out = static_cast<double>(usage.binary_object_size);
        *array_buffers_out = static_cast<double>(usage.binary_object_size);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_hash_seed(napi_env env,
                                                         uint64_t *hash_seed_out)
    {
        if (!CheckEnv(env) || hash_seed_out == nullptr)
            return napi_invalid_arg;
        *hash_seed_out = EnsureEnvState(env).hash_seed;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_statistics(
        napi_env env,
        unofficial_napi_heap_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->total_heap_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->used_heap_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.memory_used_size));
        stats_out->malloced_memory = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->peak_malloced_memory = stats_out->malloced_memory;
        stats_out->external_memory = static_cast<uint64_t>(std::max<int64_t>(0, usage.binary_object_size));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_space_count(
        napi_env env,
        uint32_t *count_out)
    {
        if (!CheckEnv(env) || count_out == nullptr)
            return napi_invalid_arg;
        *count_out = 1;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_space_statistics(
        napi_env env,
        uint32_t space_index,
        unofficial_napi_heap_space_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        if (space_index != 0)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        std::strncpy(stats_out->space_name, "quickjs", sizeof(stats_out->space_name) - 1);
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->space_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->space_used_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.memory_used_size));
        stats_out->physical_space_size = stats_out->space_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_code_statistics(
        napi_env env,
        unofficial_napi_heap_code_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->code_and_metadata_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.js_func_code_size));
        stats_out->bytecode_and_metadata_size = stats_out->code_and_metadata_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_start_cpu_profile(
        napi_env env,
        unofficial_napi_cpu_profile_start_result *result_out,
        uint32_t *profile_id_out)
    {
        if (!CheckEnv(env) || result_out == nullptr || profile_id_out == nullptr)
            return napi_invalid_arg;
        *result_out = unofficial_napi_cpu_profile_start_ok;
        *profile_id_out = 1;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_stop_cpu_profile(
        napi_env env,
        uint32_t profile_id,
        bool *found_out,
        char **json_out,
        size_t *json_len_out)
    {
        (void)profile_id;
        if (!CheckEnv(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *found_out = false;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_start_heap_profile(
        napi_env env,
        bool *started_out)
    {
        if (!CheckEnv(env) || started_out == nullptr)
            return napi_invalid_arg;
        *started_out = false;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_stop_heap_profile(
        napi_env env,
        bool *found_out,
        char **json_out,
        size_t *json_len_out)
    {
        if (!CheckEnv(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *found_out = false;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_take_heap_snapshot(
        napi_env env,
        const unofficial_napi_heap_snapshot_options *options,
        char **json_out,
        size_t *json_len_out)
    {
        (void)options;
        if (!CheckEnv(env) || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_generic_failure;
    }

    void NAPI_CDECL unofficial_napi_free_buffer(void *data)
    {
        std::free(data);
    }

    napi_status NAPI_CDECL unofficial_napi_get_continuation_preserved_embedder_data(
        napi_env env,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        if (JS_IsUndefined(state.continuation_preserved_embedder_data))
            return CreateUndefined(env, result_out);
        return WrapDup(env, state.continuation_preserved_embedder_data, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
        napi_env env,
        napi_value value)
    {
        if (!CheckEnv(env) || value == nullptr)
            return napi_invalid_arg;
        auto &state = EnsureEnvState(env);
        if (!JS_IsUndefined(state.continuation_preserved_embedder_data))
            JS_FreeValue(Ctx(env), state.continuation_preserved_embedder_data);
        state.continuation_preserved_embedder_data = JS_DupValue(Ctx(env), value->get_inner());
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env,
                                                                 napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return WrapOwned(env, JS_NewObject(Ctx(env)), result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_make_context(
        napi_env env,
        napi_value sandbox_or_symbol,
        napi_value name,
        napi_value origin_or_undefined,
        bool allow_code_gen_strings,
        bool allow_code_gen_wasm,
        bool own_microtask_queue,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)name;
        (void)origin_or_undefined;
        (void)allow_code_gen_strings;
        (void)allow_code_gen_wasm;
        (void)own_microtask_queue;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || sandbox_or_symbol == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_symbol->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(Ctx(env), sandbox, "__quickjs_contextified", JS_NewBool(Ctx(env), true));
        JS_SetPropertyStr(Ctx(env), sandbox, "globalThis", JS_DupValue(Ctx(env), sandbox));
        return WrapDup(env, sandbox, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
        napi_env env,
        napi_value sandbox_or_null,
        napi_value source,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        int64_t timeout,
        bool display_errors,
        bool break_on_sigint,
        bool break_on_first_line,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)line_offset;
        (void)column_offset;
        (void)timeout;
        (void)display_errors;
        (void)break_on_sigint;
        (void)break_on_first_line;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || source == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()) &&
            !IsTruthyProperty(env, sandbox_or_null, "__quickjs_contextified"))
            return napi_invalid_arg;

        std::string src = ToUtf8(env, source);
        std::string label = filename == nullptr ? "<contextify>" : ToUtf8(env, filename);
        JSValue result = JS_UNDEFINED;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()))
        {
            const char *wrapper_source = "(function(__sandbox, __source) { with (__sandbox) { return eval(__source); } })";
            JSValue wrapper = JS_Eval(Ctx(env),
                                      wrapper_source,
                                      std::strlen(wrapper_source),
                                      "<contextify-wrapper>",
                                      JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(wrapper))
                return napi_pending_exception;
            JSValue argv[] = {sandbox_or_null->get_inner(), source->get_inner()};
            result = JS_Call(Ctx(env), wrapper, JS_UNDEFINED, 2, argv);
            JS_FreeValue(Ctx(env), wrapper);
        }
        else
        {
            result = JS_Eval(Ctx(env), src.c_str(), src.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(result))
            return napi_pending_exception;
        return WrapOwned(env, result, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
        napi_env env,
        napi_value sandbox_or_context_global)
    {
        if (!CheckEnv(env) || sandbox_or_context_global == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_context_global->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(Ctx(env), sandbox, "__quickjs_contextified", JS_NewBool(Ctx(env), false));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
        napi_env env,
        napi_value code,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        napi_value cached_data_or_undefined,
        bool produce_cached_data,
        napi_value parsing_context_or_undefined,
        napi_value context_extensions_or_undefined,
        napi_value params_or_undefined,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)cached_data_or_undefined;
        (void)produce_cached_data;
        (void)parsing_context_or_undefined;
        (void)context_extensions_or_undefined;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;

        std::vector<JSValue> argv;
        if (params_or_undefined != nullptr && JS_IsArray(params_or_undefined->get_inner()))
        {
            uint32_t length = 0;
            JSValue len_val = JS_GetPropertyStr(Ctx(env), params_or_undefined->get_inner(), "length");
            JS_ToUint32(Ctx(env), &length, len_val);
            JS_FreeValue(Ctx(env), len_val);
            for (uint32_t i = 0; i < length; ++i)
            {
                JSValue param = JS_GetPropertyUint32(Ctx(env), params_or_undefined->get_inner(), i);
                argv.push_back(param);
            }
        }

        std::string source = ToUtf8(env, code);
        std::string source_url;
        JSValue code_arg = JS_DupValue(Ctx(env), code->get_inner());
        if (filename != nullptr && !JS_IsUndefined(filename->get_inner()) && !JS_IsNull(filename->get_inner()))
        {
            source_url = ToUtf8(env, filename);
            if (!source.empty() && !source_url.empty())
            {
                source += "\n//# sourceURL=";
                source += source_url;
                JSValue with_source_url =
                    JS_NewStringLen(Ctx(env), source.c_str(), source.size());
                if (!JS_IsException(with_source_url))
                {
                    JS_FreeValue(Ctx(env), code_arg);
                    code_arg = with_source_url;
                }
            }
        }
        argv.push_back(code_arg);

        JSValue global = JS_GetGlobalObject(Ctx(env));
        JSValue function_ctor = JS_GetPropertyStr(Ctx(env), global, "Function");
        JS_FreeValue(Ctx(env), global);
        JSValue fn = JS_CallConstructor(Ctx(env), function_ctor, static_cast<int>(argv.size()), argv.data());
        JS_FreeValue(Ctx(env), function_ctor);
        for (JSValue arg : argv)
            JS_FreeValue(Ctx(env), arg);
        if (JS_IsException(fn))
        {
            JSValue exc = JS_GetException(Ctx(env));
            AnnotateContextifyCompileException(env, exc, source, source_url, line_offset, column_offset);
            napi_util__::set_last_exception(env, exc);
            return napi_pending_exception;
        }

        JSValue out = JS_NewObject(Ctx(env));
        JS_SetPropertyStr(Ctx(env), out, "function", fn);
        if (!source_url.empty())
            SetStringProperty(Ctx(env), out, "sourceURL", source_url);
        JS_SetPropertyStr(Ctx(env), out, "sourceMapURL", JS_UNDEFINED);
        return WrapOwned(env, out, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
        napi_env env,
        napi_value code,
        napi_value filename,
        napi_value resource_name_or_undefined,
        bool cjs_var_in_scope,
        bool *result_out)
    {
        (void)filename;
        (void)resource_name_or_undefined;
        (void)cjs_var_in_scope;
        if (!CheckEnv(env) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        std::string src = ToUtf8(env, code);
        *result_out = src.find("export ") != std::string::npos ||
                      src.find("import ") != std::string::npos ||
                      src.find("import(") != std::string::npos;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_create_cached_data(
        napi_env env,
        napi_value code,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        napi_value host_defined_option_id,
        napi_value *cached_data_buffer_out)
    {
        (void)code;
        (void)filename;
        (void)line_offset;
        (void)column_offset;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || cached_data_buffer_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, cached_data_buffer_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_source_text(
        napi_env env,
        napi_value wrapper,
        napi_value url,
        napi_value context_or_undefined,
        napi_value source,
        int32_t line_offset,
        int32_t column_offset,
        napi_value cached_data_or_id,
        void **handle_out)
    {
        (void)wrapper;
        (void)url;
        (void)context_or_undefined;
        (void)source;
        (void)line_offset;
        (void)column_offset;
        (void)cached_data_or_id;
        if (!CheckEnv(env) || handle_out == nullptr)
            return napi_invalid_arg;
        *handle_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_synthetic(
        napi_env env,
        napi_value wrapper,
        napi_value url,
        napi_value context_or_undefined,
        napi_value export_names,
        napi_value synthetic_eval_steps,
        void **handle_out)
    {
        (void)wrapper;
        (void)url;
        (void)context_or_undefined;
        (void)export_names;
        (void)synthetic_eval_steps;
        if (!CheckEnv(env) || handle_out == nullptr)
            return napi_invalid_arg;
        *handle_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_destroy(napi_env env, void *handle)
    {
        (void)handle;
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_requests(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateEmptyArray(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_link(
        napi_env env,
        void *handle,
        size_t count,
        void *const *linked_handles)
    {
        (void)handle;
        (void)count;
        (void)linked_handles;
        return UnsupportedIfValidEnv(env);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_instantiate(napi_env env, void *handle)
    {
        (void)handle;
        return UnsupportedIfValidEnv(env);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate(
        napi_env env,
        void *handle,
        int64_t timeout,
        bool break_on_sigint,
        napi_value *result_out)
    {
        (void)handle;
        (void)timeout;
        (void)break_on_sigint;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate_sync(
        napi_env env,
        void *handle,
        napi_value filename,
        napi_value parent_filename,
        napi_value *result_out)
    {
        (void)handle;
        (void)filename;
        (void)parent_filename;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_namespace(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return WrapOwned(env, JS_NewObject(Ctx(env)), result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_status(
        napi_env env,
        void *handle,
        int32_t *status_out)
    {
        (void)handle;
        if (!CheckEnv(env) || status_out == nullptr)
            return napi_invalid_arg;
        *status_out = 0;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_error(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_has_top_level_await(
        napi_env env,
        void *handle,
        bool *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        *result_out = false;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_has_async_graph(
        napi_env env,
        void *handle,
        bool *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        *result_out = false;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_check_unsettled_top_level_await(
        napi_env env,
        napi_value module_wrap,
        bool warnings,
        bool *settled_out)
    {
        (void)module_wrap;
        (void)warnings;
        if (!CheckEnv(env) || settled_out == nullptr)
            return napi_invalid_arg;
        *settled_out = true;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_export(
        napi_env env,
        void *handle,
        napi_value export_name,
        napi_value export_value)
    {
        (void)handle;
        (void)export_name;
        (void)export_value;
        return UnsupportedIfValidEnv(env);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_module_source_object(
        napi_env env,
        void *handle,
        napi_value source_object)
    {
        (void)handle;
        (void)source_object;
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_source_object(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_cached_data(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_import_module_dynamically_callback(
        napi_env env,
        napi_value callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return StoreOptionalFunction(env, callback, &EnsureEnvState(env).import_module_dynamically_callback);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
        napi_env env,
        napi_value callback)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return StoreOptionalFunction(env, callback, &EnsureEnvState(env).initialize_import_meta_object_callback);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_required_module_facade(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_destroy_env_instance_for_testing(napi_env env)
    {
        if (env == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = env->context();
        napi_status status = DestroyEnvInstance(env);
        JS_FreeContext(ctx);
        return status;
    }

} // extern "C"
