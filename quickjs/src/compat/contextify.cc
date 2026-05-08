#include "compat/contextify.h"

#include "compat/quickjs_utilities.h"
#include "internal/quickjs_trace.h"

#include <cstdio>

namespace quickjs::detail
{
    // Brief: ContextifyCompileTraceEnabled belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool ContextifyCompileTraceEnabled()
    {
        return EDGE_TRACE_ENABLED("EDGE_TRACE_QUICKJS_CONTEXTIFY") ||
               EDGE_TRACE_ENABLED("EDGE_TRACE_BUILTINS");
    }

    // Brief: GetInt32PropertyOr belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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

    // Brief: GetStringPropertyOrEmpty belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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

    // Brief: BuiltinIdFromResourceName belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string BuiltinIdFromResourceName(const std::string &resource_name)
    {
        const char prefix[] = "node:";
        if (resource_name.rfind(prefix, 0) == 0)
            return resource_name.substr(sizeof(prefix) - 1);
        return {};
    }

    // Brief: SourceLineAt belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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

    // Brief: SetInt32Property belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void SetInt32Property(JSContext *ctx, JSValueConst object, const char *name, int32_t value)
    {
        JS_SetPropertyStr(ctx, object, name, JS_NewInt32(ctx, value));
    }

    // Brief: AnnotateContextifyCompileException belongs to the contextify compile diagnostics compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
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
}
