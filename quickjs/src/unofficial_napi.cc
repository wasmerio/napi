#include "internal/quickjs_env.h"

#include <unordered_map>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

struct UnofficialEnvScope
{
    JSRuntime *rt;
    JSContext *ctx;
    napi_env env;
};

namespace
{
    std::string ToUtf8(napi_env env, napi_value value)
    {
        if (env == nullptr || value == nullptr)
            return {};
        const char *str = JS_ToCString(env->ctx, value->get_inner());
        if (str == nullptr)
            return {};
        std::string out(str);
        JS_FreeCString(env->ctx, str);
        return out;
    }

    bool IsTruthyProperty(napi_env env, napi_value object, const char *name)
    {
        JSValue prop = JS_GetPropertyStr(env->ctx, object->get_inner(), name);
        if (JS_IsException(prop))
            return false;
        bool out = JS_ToBool(env->ctx, prop);
        JS_FreeValue(env->ctx, prop);
        return out;
    }

    struct ErrorFormattingState
    {
        bool source_maps_enabled = false;
        JSValue get_source_map_error_source = JS_UNDEFINED;
    };

    std::mutex g_error_formatting_mu;
    std::unordered_map<napi_env, ErrorFormattingState> g_error_formatting_states;

    void ClearErrorFormattingState(napi_env env)
    {
        std::lock_guard<std::mutex> lock(g_error_formatting_mu);
        auto it = g_error_formatting_states.find(env);
        if (it == g_error_formatting_states.end())
            return;
        if (!JS_IsUndefined(it->second.get_source_map_error_source))
        {
            JS_FreeValue(env->ctx, it->second.get_source_map_error_source);
        }
        g_error_formatting_states.erase(it);
    }
}

extern "C"
{

    napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
        JSContext *context, int32_t module_api_version, napi_env *result)
    {
        if (result == nullptr || context == nullptr)
            return napi_invalid_arg;

        // TODO: find out if we need to set that somehow
        // context->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);

        auto rt = JS_GetRuntime(context);
        if (0 != RegisterExternalClass(rt)) {
            return napi_generic_failure;
        }

        auto env = new (std::nothrow) napi_env__(context, module_api_version);
        if (env == nullptr)
            return napi_generic_failure;
        if (env->root_scope == nullptr)
        {
            delete env;
            return napi_generic_failure;
        }
        
        JS_SetContextOpaque(context, env);

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

    napi_status NAPI_CDECL unofficial_napi_destroy_env_instance_for_testing(napi_env env)
    {
        // TODO: gracefull shutdown
        ClearErrorFormattingState(env);
        JSContext *ctx = env->ctx;
        JS_SetContextOpaque(ctx, nullptr);
        delete env;
        JS_FreeContext(ctx);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(
        napi_env env,
        bool enabled)
    {
        if (env == nullptr || env->ctx == nullptr)
            return napi_invalid_arg;

        std::lock_guard<std::mutex> lock(g_error_formatting_mu);
        g_error_formatting_states[env].source_maps_enabled = enabled;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(
        napi_env env,
        napi_value callback)
    {
        if (env == nullptr || env->ctx == nullptr)
            return napi_invalid_arg;

        std::lock_guard<std::mutex> lock(g_error_formatting_mu);
        auto &state = g_error_formatting_states[env];
        if (!JS_IsUndefined(state.get_source_map_error_source))
        {
            JS_FreeValue(env->ctx, state.get_source_map_error_source);
            state.get_source_map_error_source = JS_UNDEFINED;
        }
        if (callback != nullptr)
        {
            JSValue value = callback->get_inner();
            if (!JS_IsFunction(env->ctx, value))
                return napi_invalid_arg;
            state.get_source_map_error_source = JS_DupValue(env->ctx, value);
        }
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(
        napi_env env,
        napi_value error)
    {
        if (env == nullptr || env->ctx == nullptr || error == nullptr)
            return napi_invalid_arg;

        JSValue callback = JS_UNDEFINED;
        {
            std::lock_guard<std::mutex> lock(g_error_formatting_mu);
            auto it = g_error_formatting_states.find(env);
            if (it == g_error_formatting_states.end() ||
                !it->second.source_maps_enabled ||
                JS_IsUndefined(it->second.get_source_map_error_source))
            {
                return napi_ok;
            }
            callback = JS_DupValue(env->ctx, it->second.get_source_map_error_source);
        }

        JSValue mapped = JS_Call(env->ctx, callback, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(env->ctx, callback);
        if (JS_IsException(mapped))
        {
            JSValue exc = JS_GetException(env->ctx);
            JS_FreeValue(env->ctx, exc);
            return napi_generic_failure;
        }

        if (JS_IsString(mapped))
        {
            JS_SetPropertyStr(env->ctx,
                              error->get_inner(),
                              "node:arrowMessage",
                              mapped);
        }
        else
        {
            JS_FreeValue(env->ctx, mapped);
        }

        return napi_ok;
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
        if (env == nullptr || env->ctx == nullptr || sandbox_or_symbol == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_symbol->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(env->ctx, sandbox, "__quickjs_contextified", JS_NewBool(env->ctx, true));
        JS_SetPropertyStr(env->ctx, sandbox, "globalThis", JS_DupValue(env->ctx, sandbox));
        *result_out = env->current_scope->wrap_value(JS_DupValue(env->ctx, sandbox), true);
        return (*result_out == nullptr) ? napi_generic_failure : napi_ok;
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
        if (env == nullptr || env->ctx == nullptr || source == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()) &&
            !IsTruthyProperty(env, sandbox_or_null, "__quickjs_contextified"))
            return napi_invalid_arg;

        std::string src = ToUtf8(env, source);
        std::string label = filename == nullptr ? "<contextify>" : ToUtf8(env, filename);
        JSValue result = JS_UNDEFINED;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()))
        {
            JSValue wrapper = JS_Eval(env->ctx,
                                      "(function(__sandbox, __source) { with (__sandbox) { return eval(__source); } })",
                                      std::strlen("(function(__sandbox, __source) { with (__sandbox) { return eval(__source); } })"),
                                      "<contextify-wrapper>",
                                      JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(wrapper))
                return napi_pending_exception;
            JSValue argv[] = {sandbox_or_null->get_inner(), source->get_inner()};
            result = JS_Call(env->ctx, wrapper, JS_UNDEFINED, 2, argv);
            JS_FreeValue(env->ctx, wrapper);
        }
        else
        {
            result = JS_Eval(env->ctx, src.c_str(), src.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(result))
            return napi_pending_exception;
        *result_out = env->current_scope->wrap_value(result, true);
        return (*result_out == nullptr) ? napi_generic_failure : napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
        napi_env env,
        napi_value sandbox_or_context_global)
    {
        if (env == nullptr || env->ctx == nullptr || sandbox_or_context_global == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_context_global->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(env->ctx, sandbox, "__quickjs_contextified", JS_NewBool(env->ctx, false));
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
        (void)filename;
        (void)line_offset;
        (void)column_offset;
        (void)cached_data_or_undefined;
        (void)produce_cached_data;
        (void)parsing_context_or_undefined;
        (void)context_extensions_or_undefined;
        (void)host_defined_option_id;
        if (env == nullptr || env->ctx == nullptr || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;

        std::vector<JSValue> argv;
        if (params_or_undefined != nullptr && JS_IsArray(params_or_undefined->get_inner()))
        {
            uint32_t length = 0;
            JSValue len_val = JS_GetPropertyStr(env->ctx, params_or_undefined->get_inner(), "length");
            JS_ToUint32(env->ctx, &length, len_val);
            JS_FreeValue(env->ctx, len_val);
            for (uint32_t i = 0; i < length; ++i)
            {
                JSValue param = JS_GetPropertyUint32(env->ctx, params_or_undefined->get_inner(), i);
                argv.push_back(param);
            }
        }
        argv.push_back(code->get_inner());

        JSValue global = JS_GetGlobalObject(env->ctx);
        JSValue function_ctor = JS_GetPropertyStr(env->ctx, global, "Function");
        JS_FreeValue(env->ctx, global);
        JSValue fn = JS_CallConstructor(env->ctx, function_ctor, static_cast<int>(argv.size()), argv.data());
        JS_FreeValue(env->ctx, function_ctor);
        for (size_t i = 0; i + 1 < argv.size(); ++i)
            JS_FreeValue(env->ctx, argv[i]);
        if (JS_IsException(fn))
            return napi_pending_exception;

        JSValue out = JS_NewObject(env->ctx);
        JS_SetPropertyStr(env->ctx, out, "function", fn);
        *result_out = env->current_scope->wrap_value(out, true);
        return (*result_out == nullptr) ? napi_generic_failure : napi_ok;
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
        if (env == nullptr || code == nullptr || result_out == nullptr)
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
        if (env == nullptr || cached_data_buffer_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, cached_data_buffer_out);
    }

} // extern "C"
