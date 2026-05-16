#include "internal/napi_contextify.h"

#include "internal/napi_env.h"
#include "internal/napi_util.h"
#include "internal/napi_value.h"
#include "internal/quickjs_trace.h"
#include "node_api.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace quickjs::detail
{
    namespace
    {
        constexpr int k_contextify_internal_property_flags =
            JS_PROP_HAS_VALUE |
            JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE |
            JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE |
            JS_PROP_HAS_ENUMERABLE;

        napi_status define_contextify_internal_property(napi_env env,
                                                        JSContext *ctx,
                                                        JSValueConst object,
                                                        const char *name,
                                                        JSValue value)
        {
            if (JS_DefinePropertyValueStr(ctx, object, name, value, k_contextify_internal_property_flags) < 0)
                return napi_util__::return_pending_if_caught(env, "Failed to define contextify property");
            return napi_ok;
        }
    } // namespace

    napi_contextify__::napi_contextify__(napi_env env, JSContext *context)
        : env_{env},
          ctx_{context},
          source_map_error_source_callback_{JS_UNDEFINED}
    {
    }

    napi_contextify__::~napi_contextify__()
    {
        teardown();
    }

    void napi_contextify__::teardown()
    {
        if (torn_down_)
            return;
        JS_FreeValue(ctx_, source_map_error_source_callback_);
        source_map_error_source_callback_ = JS_UNDEFINED;
        torn_down_ = true;
    }

    bool napi_contextify__::compile_trace_enabled() const
    {
        return NAPI_QUICKJS_TRACE_ENABLED("NAPI_QUICKJS_TRACE_CONTEXTIFY") ||
               NAPI_QUICKJS_TRACE_ENABLED("NAPI_QUICKJS_TRACE_BUILTINS");
    }

    int32_t napi_contextify__::get_int32_property_or(JSValueConst object,
                                                     const char *name,
                                                     int32_t fallback) const
    {
        JSValue value = JS_GetPropertyStr(ctx_, object, name);
        if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value))
        {
            JS_FreeValue(ctx_, value);
            return fallback;
        }
        int32_t out = fallback;
        (void)JS_ToInt32(ctx_, &out, value);
        JS_FreeValue(ctx_, value);
        return out;
    }

    std::string napi_contextify__::get_string_property_or_empty(JSValueConst object,
                                                                const char *name) const
    {
        JSValue value = JS_GetPropertyStr(ctx_, object, name);
        if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value))
        {
            JS_FreeValue(ctx_, value);
            return {};
        }
        std::string out = napi_util__::to_utf8(ctx_, value);
        JS_FreeValue(ctx_, value);
        return out;
    }

    std::string napi_contextify__::builtin_id_from_resource_name(const std::string &resource_name) const
    {
        const char prefix[] = "node:";
        if (resource_name.rfind(prefix, 0) == 0)
            return resource_name.substr(sizeof(prefix) - 1);
        return {};
    }

    std::string napi_contextify__::source_line_at(const std::string &source,
                                                  int32_t one_based_line) const
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

    std::string napi_contextify__::prepare_function_body_source(const std::string &source) const
    {
        if (source.size() < 2 || source[0] != '#' || source[1] != '!')
            return source;

        // V8 accepts hashbangs in vm.compileFunction bodies. QuickJS only skips
        // them for whole-script eval, so preserve lines while hiding the text.
        std::string prepared = source;
        for (char &ch : prepared)
        {
            if (ch == '\n' || ch == '\r')
                break;
            ch = ' ';
        }
        return prepared;
    }

    JSValue napi_contextify__::compile_cjs_function(const std::string &source,
                                                    const std::string &source_url,
                                                    const std::vector<std::string> &params,
                                                    std::string *diagnostic_source_out) const
    {
        std::string function_body = prepare_function_body_source(source);
        std::string diagnostic_source = source;
        if (!source.empty() && !source_url.empty())
        {
            function_body += "\n//# sourceURL=";
            function_body += source_url;
            diagnostic_source += "\n//# sourceURL=";
            diagnostic_source += source_url;
        }
        if (diagnostic_source_out != nullptr)
            *diagnostic_source_out = diagnostic_source;

        std::string compile_source = "(function anonymous(";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i != 0)
                compile_source += ',';
            compile_source += params[i];
        }
        compile_source += "\n) {\n";
        compile_source += function_body;
        compile_source += "\n})";

        JSEvalOptions options = {
            .version = JS_EVAL_OPTIONS_VERSION,
            .eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY,
            .filename = source_url.empty() ? "<contextify>" : source_url.c_str(),
            .line_num = 1,
        };
        JSValue bytecode = JS_Eval2(ctx_, compile_source.c_str(), compile_source.size(), &options);
        if (JS_IsException(bytecode))
            return JS_EXCEPTION;
        return JS_EvalFunction(ctx_, bytecode);
    }

    bool napi_contextify__::can_parse_as_module(const std::string &source,
                                                const std::string &source_url) const
    {
        JSEvalOptions options = {
            .version = JS_EVAL_OPTIONS_VERSION,
            .eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
            .filename = source_url.empty() ? "<module>" : source_url.c_str(),
            .line_num = 1,
        };
        JSValue module = JS_Eval2(ctx_, source.c_str(), source.size(), &options);
        if (JS_IsException(module))
        {
            JSValue exception = JS_GetException(ctx_);
            JS_FreeValue(ctx_, exception);
            return false;
        }
        JS_FreeValue(ctx_, module);
        return true;
    }

    void napi_contextify__::set_int32_property(JSValueConst object,
                                               const char *name,
                                               int32_t value) const
    {
        JS_SetPropertyStr(ctx_, object, name, JS_NewInt32(ctx_, value));
    }

    void napi_contextify__::annotate_compile_exception(JSValueConst exception,
                                                       const std::string &source,
                                                       const std::string &resource_name,
                                                       int32_t line_offset,
                                                       int32_t column_offset)
    {
        if (!napi_util__::check_env(env_) || !JS_IsObject(exception))
            return;

        const std::string builtin_id = builtin_id_from_resource_name(resource_name);
        const std::string quickjs_file = get_string_property_or_empty(exception, "fileName");
        const int32_t quickjs_line = get_int32_property_or(exception, "lineNumber", -1);
        const int32_t mapped_line = quickjs_line > 0 ? quickjs_line + line_offset : -1;

        JS_SetPropertyStr(ctx_, exception, "node:quickjsContextifyCompile", JS_NewBool(ctx_, true));
        napi_util__::set_string_property(ctx_, exception, "node:quickjsCompileResourceName", resource_name);
        if (!builtin_id.empty())
            napi_util__::set_string_property(ctx_, exception, "node:quickjsCompileBuiltinId", builtin_id);
        set_int32_property(exception, "node:quickjsCompileLineOffset", line_offset);
        set_int32_property(exception, "node:quickjsCompileColumnOffset", column_offset);
        if (quickjs_line > 0)
            set_int32_property(exception, "node:quickjsCompileQuickJSLine", quickjs_line);
        if (mapped_line > 0)
            set_int32_property(exception, "node:quickjsCompileMappedLine", mapped_line);

        if (!compile_trace_enabled())
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

        std::string source_line = source_line_at(source, quickjs_line);
        if (!source_line.empty())
            summary += " sourceLine=\"" + source_line + "\"";

        std::fprintf(stderr, "%s\n", summary.c_str());

        JSValue stack = JS_GetPropertyStr(ctx_, exception, "stack");
        std::string stack_text;
        if (!JS_IsException(stack) && !JS_IsUndefined(stack) && !JS_IsNull(stack))
            stack_text = napi_util__::to_utf8(ctx_, stack);
        JS_FreeValue(ctx_, stack);
        if (!stack_text.empty())
            napi_util__::set_string_property(ctx_, exception, "stack", summary + "\n" + stack_text);
    }

    napi_status napi_contextify__::get_error_source_positions(
        napi_value error,
        unofficial_napi_error_source_positions *out)
    {
        if (!napi_util__::check_env(env_) || error == nullptr || out == nullptr)
            return napi_invalid_arg;
        std::memset(out, 0, sizeof(*out));
        out->line_number = -1;
        out->start_column = -1;
        out->end_column = -1;
        napi_value empty = nullptr;
        napi_status status = napi_create_string_utf8(env_, "", 0, &empty);
        if (status != napi_ok)
            return status;
        out->source_line = empty;
        out->script_resource_name = empty;
        return napi_ok;
    }

    napi_status napi_contextify__::preserve_error_source_message(napi_value error)
    {
        // QuickJS does not expose a V8-style message for an arbitrary caught Error.
        if (!napi_util__::check_env(env_) || error == nullptr)
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status napi_contextify__::set_source_maps_enabled(bool enabled)
    {
        if (!napi_util__::check_env(env_))
            return napi_invalid_arg;
        source_maps_enabled_ = enabled;
        return napi_ok;
    }

    napi_status napi_contextify__::set_get_source_map_error_source_callback(napi_value callback)
    {
        if (!napi_util__::check_env(env_))
            return napi_invalid_arg;
        if (callback != nullptr && !JS_IsUndefined(napi_quickjs_value_inner(env_, callback)) &&
            !JS_IsNull(napi_quickjs_value_inner(env_, callback)) && !JS_IsFunction(ctx_, napi_quickjs_value_inner(env_, callback)))
        {
            return napi_invalid_arg;
        }

        JS_FreeValue(ctx_, source_map_error_source_callback_);
        source_map_error_source_callback_ =
            callback == nullptr ? JS_UNDEFINED : JS_DupValue(ctx_, napi_quickjs_value_inner(env_, callback));
        return napi_ok;
    }

    napi_status napi_contextify__::get_error_source_line_for_stderr(napi_value error,
                                                                    napi_value *result_out)
    {
        if (!napi_util__::check_env(env_) || error == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue value = JS_GetPropertyStr(ctx_, napi_quickjs_value_inner(env_, error), "node:arrowMessage");
        if (JS_IsException(value))
            return napi_pending_exception;
        if (JS_IsUndefined(value))
        {
            JS_FreeValue(ctx_, value);
            return napi_util__::create_undefined(env_, result_out);
        }
        return napi_util__::wrap_owned(env_, value, result_out);
    }

    napi_status napi_contextify__::get_error_thrown_at(napi_value error,
                                                       napi_value *result_out)
    {
        (void)error;
        if (!napi_util__::check_env(env_) || result_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_undefined(env_, result_out);
    }

    napi_status napi_contextify__::take_preserved_error_formatting(napi_value error,
                                                                   napi_value *source_line_out,
                                                                   napi_value *thrown_at_out)
    {
        if (!napi_util__::check_env(env_) || error == nullptr || source_line_out == nullptr || thrown_at_out == nullptr)
            return napi_invalid_arg;
        napi_status status = get_error_source_line_for_stderr(error, source_line_out);
        if (status != napi_ok)
            return status;
        return napi_util__::create_undefined(env_, thrown_at_out);
    }

    napi_status napi_contextify__::make_context(napi_value sandbox_or_symbol,
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
        if (!napi_util__::check_env(env_) || sandbox_or_symbol == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = napi_quickjs_value_inner(env_, sandbox_or_symbol);
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        napi_status status = define_contextify_internal_property(env_,
                                                                 ctx_,
                                                                 sandbox,
                                                                 "__quickjs_contextified",
                                                                 JS_NewBool(ctx_, true));
        if (status != napi_ok)
            return status;
        status = define_contextify_internal_property(env_,
                                                     ctx_,
                                                     sandbox,
                                                     "globalThis",
                                                     JS_DupValue(ctx_, sandbox));
        if (status != napi_ok)
            return status;
        return napi_util__::wrap_dup(env_, sandbox, result_out);
    }

    napi_status napi_contextify__::run_script(napi_value sandbox_or_null,
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
        if (!napi_util__::check_env(env_) || source == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        env_->module_wrap().register_dynamic_import_referrer(filename, host_defined_option_id);
        if (sandbox_or_null != nullptr && !JS_IsNull(napi_quickjs_value_inner(env_, sandbox_or_null)) &&
            !napi_util__::is_truthy_property(env_, sandbox_or_null, "__quickjs_contextified"))
        {
            env_->module_wrap().unregister_dynamic_import_referrer(filename, host_defined_option_id);
            return napi_invalid_arg;
        }

        std::string src = napi_util__::to_utf8(env_, source);
        std::string label = filename == nullptr ? "<contextify>" : napi_util__::to_utf8(env_, filename);
        JSValue result = JS_UNDEFINED;
        if (sandbox_or_null != nullptr && !JS_IsNull(napi_quickjs_value_inner(env_, sandbox_or_null)))
        {
            const char *wrapper_source = "(function(__sandbox, __source) { with (__sandbox) { return eval(__source); } })";
            JSValue wrapper = JS_Eval(ctx_,
                                      wrapper_source,
                                      std::strlen(wrapper_source),
                                      "<contextify-wrapper>",
                                      JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(wrapper))
            {
                env_->module_wrap().unregister_dynamic_import_referrer(filename, host_defined_option_id);
                return napi_pending_exception;
            }
            JSValue argv[] = {napi_quickjs_value_inner(env_, sandbox_or_null), napi_quickjs_value_inner(env_, source)};
            result = JS_Call(ctx_, wrapper, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx_, wrapper);
        }
        else
        {
            result = JS_Eval(ctx_, src.c_str(), src.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
        }
        env_->module_wrap().unregister_dynamic_import_referrer(filename, host_defined_option_id);
        if (JS_IsException(result))
            return napi_pending_exception;
        return napi_util__::wrap_owned(env_, result, result_out);
    }

    napi_status napi_contextify__::dispose_context(napi_value sandbox_or_context_global)
    {
        if (!napi_util__::check_env(env_) || sandbox_or_context_global == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = napi_quickjs_value_inner(env_, sandbox_or_context_global);
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        return define_contextify_internal_property(env_,
                                                   ctx_,
                                                   sandbox,
                                                   "__quickjs_contextified",
                                                   JS_NewBool(ctx_, false));
    }

    napi_status napi_contextify__::compile_function(napi_value code,
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
        if (!napi_util__::check_env(env_) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;

        std::vector<std::string> params;
        if (params_or_undefined != nullptr && JS_IsArray(napi_quickjs_value_inner(env_, params_or_undefined)))
        {
            uint32_t length = 0;
            JSValue len_val = JS_GetPropertyStr(ctx_, napi_quickjs_value_inner(env_, params_or_undefined), "length");
            JS_ToUint32(ctx_, &length, len_val);
            JS_FreeValue(ctx_, len_val);
            params.reserve(length);
            for (uint32_t i = 0; i < length; ++i)
            {
                JSValue param = JS_GetPropertyUint32(ctx_, napi_quickjs_value_inner(env_, params_or_undefined), i);
                if (JS_IsException(param))
                    return napi_pending_exception;
                params.push_back(napi_util__::to_utf8(ctx_, param));
                JS_FreeValue(ctx_, param);
            }
        }

        std::string source = napi_util__::to_utf8(env_, code);
        std::string source_url;
        if (filename != nullptr && !JS_IsUndefined(napi_quickjs_value_inner(env_, filename)) && !JS_IsNull(napi_quickjs_value_inner(env_, filename)))
        {
            source_url = napi_util__::to_utf8(env_, filename);
        }

        std::string diagnostic_source;
        JSValue fn = compile_cjs_function(source, source_url, params, &diagnostic_source);
        if (JS_IsException(fn))
        {
            JSValue exc = JS_GetException(ctx_);
            annotate_compile_exception(exc, diagnostic_source, source_url, line_offset, column_offset);
            napi_util__::set_last_exception(env_, exc);
            return napi_pending_exception;
        }

        JSValue out = JS_NewObject(ctx_);
        JS_SetPropertyStr(ctx_, out, "function", fn);
        if (!source_url.empty())
            napi_util__::set_string_property(ctx_, out, "sourceURL", source_url);
        JS_SetPropertyStr(ctx_, out, "sourceMapURL", JS_UNDEFINED);
        return napi_util__::wrap_owned(env_, out, result_out);
    }

    napi_status napi_contextify__::contains_module_syntax(napi_value code,
                                                          napi_value filename,
                                                          napi_value resource_name_or_undefined,
                                                          bool cjs_var_in_scope,
                                                          bool *result_out)
    {
        if (!napi_util__::check_env(env_) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;

        std::string source = napi_util__::to_utf8(env_, code);
        std::string filename_string;
        if (filename != nullptr && !JS_IsUndefined(napi_quickjs_value_inner(env_, filename)) &&
            !JS_IsNull(napi_quickjs_value_inner(env_, filename)))
            filename_string = napi_util__::to_utf8(env_, filename);

        std::string resource_name = filename_string;
        if (resource_name_or_undefined != nullptr &&
            !JS_IsUndefined(napi_quickjs_value_inner(env_, resource_name_or_undefined)) &&
            !JS_IsNull(napi_quickjs_value_inner(env_, resource_name_or_undefined)))
            resource_name = napi_util__::to_utf8(env_, resource_name_or_undefined);

        std::vector<std::string> params;
        if (cjs_var_in_scope)
            params = {"exports", "require", "module", "__filename", "__dirname"};

        JSValue fn = compile_cjs_function(source, filename_string, params, nullptr);
        if (!JS_IsException(fn))
        {
            JS_FreeValue(ctx_, fn);
            *result_out = false;
            return napi_ok;
        }

        JSValue cjs_exception = JS_GetException(ctx_);
        JS_FreeValue(ctx_, cjs_exception);

        *result_out = can_parse_as_module(source, resource_name);
        return napi_ok;
    }

    napi_status napi_contextify__::create_cached_data(napi_value code,
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
        if (!napi_util__::check_env(env_) || cached_data_buffer_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env_, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env_, napi_uint8_array, 0, arraybuffer, 0, cached_data_buffer_out);
    }
}
