#include "compat/module_loading.h"

#include "compat/quickjs_utilities.h"
#include "internal/napi_util.h"
#include "internal/quickjs_trace.h"
#include "quickjs_cjs_exports.h"
#include "unofficial_module_loader.h"

#include <algorithm>
#include <cstdio>

namespace quickjs::detail
{
    JSValue GetBuiltinModuleValue(JSContext *ctx, const std::string &specifier);

    // Brief: IsNodeBuiltinSpecifier belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsNodeBuiltinSpecifier(const std::string &specifier)
    {
        if (StartsWith(specifier, "node:"))
            return true;
        static const char *const builtins[] = {
            "assert", "buffer", "events", "fs", "module", "node:assert", "node:buffer",
            "crypto", "http", "node:events", "node:fs", "node:module", "node:os", "node:path", "node:process",
            "node:url", "node:util", "os", "path", "perf_hooks", "process", "stream", "url", "util", "zlib"};
        for (const char *builtin : builtins)
        {
            if (specifier == builtin)
                return true;
        }
        return false;
    }

    // Brief: IsRuntimePackageTarget belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool IsRuntimePackageTarget(const std::string &target)
    {
        if (target.empty())
            return false;
        if (target.find(".d.ts") != std::string::npos ||
            target.find(".d.cts") != std::string::npos ||
            target.find(".d.mts") != std::string::npos)
            return false;
        const std::filesystem::path path(target);
        const std::string ext = path.extension().string();
        return ext.empty() || ext == ".js" || ext == ".mjs" || ext == ".cjs" || ext == ".json";
    }

    // Brief: SkipJsonWhitespace belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    size_t SkipJsonWhitespace(const std::string &json, size_t pos, size_t limit)
    {
        while (pos < limit && pos < json.size())
        {
            const char ch = json[pos];
            if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
                break;
            ++pos;
        }
        return pos;
    }

    // Brief: FindJsonStringEnd belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    size_t FindJsonStringEnd(const std::string &json, size_t quote_pos, size_t limit)
    {
        bool escaped = false;
        for (size_t i = quote_pos + 1; i < limit && i < json.size(); ++i)
        {
            const char ch = json[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (ch == '\\')
            {
                escaped = true;
                continue;
            }
            if (ch == '"')
                return i;
        }
        return std::string::npos;
    }

    // Brief: JsonStringValueAt belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string JsonStringValueAt(const std::string &json, size_t quote_pos, size_t limit)
    {
        if (quote_pos >= limit || quote_pos >= json.size() || json[quote_pos] != '"')
            return {};
        const size_t end = FindJsonStringEnd(json, quote_pos, limit);
        if (end == std::string::npos)
            return {};
        return json.substr(quote_pos + 1, end - quote_pos - 1);
    }

    // Brief: FindJsonObjectEnd belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    size_t FindJsonObjectEnd(const std::string &json, size_t open_pos, size_t limit)
    {
        if (open_pos >= limit || open_pos >= json.size() || json[open_pos] != '{')
            return std::string::npos;

        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t i = open_pos; i < limit && i < json.size(); ++i)
        {
            const char ch = json[i];
            if (in_string)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (ch == '\\')
                {
                    escaped = true;
                }
                else if (ch == '"')
                {
                    in_string = false;
                }
                continue;
            }
            if (ch == '"')
            {
                in_string = true;
                continue;
            }
            if (ch == '{')
            {
                ++depth;
            }
            else if (ch == '}')
            {
                if (depth == 0)
                    return std::string::npos;
                --depth;
                if (depth == 0)
                    return i + 1;
            }
        }
        return std::string::npos;
    }

    // Brief: JsonTargetAfterCondition belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string JsonTargetAfterCondition(const std::string &json,
                                         size_t range_start,
                                         size_t range_end,
                                         const char *condition)
    {
        const std::string key = std::string("\"") + condition + "\"";
        for (size_t condition_pos = json.find(key, range_start);
             condition_pos != std::string::npos && condition_pos < range_end;
             condition_pos = json.find(key, condition_pos + key.size()))
        {
            const size_t colon = json.find(':', condition_pos + key.size());
            if (colon == std::string::npos || colon >= range_end)
                return {};
            const size_t value_pos = SkipJsonWhitespace(json, colon + 1, range_end);
            if (value_pos >= range_end || value_pos >= json.size())
                return {};
            if (json[value_pos] == '"')
                return JsonStringValueAt(json, value_pos, range_end);
            if (json[value_pos] == '{')
            {
                const size_t object_end = FindJsonObjectEnd(json, value_pos, range_end);
                if (object_end == std::string::npos)
                    return {};
                const std::string nested_default = JsonTargetAfterCondition(json,
                                                                            value_pos + 1,
                                                                            object_end - 1,
                                                                            "default");
                if (!nested_default.empty())
                    return nested_default;
            }
        }
        return {};
    }

    // Brief: JsonDirectStringValueAfterKey belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string JsonDirectStringValueAfterKey(const std::string &json,
                                              size_t key_pos,
                                              size_t key_size,
                                              size_t range_end)
    {
        const size_t colon = json.find(':', key_pos + key_size);
        if (colon == std::string::npos || colon >= range_end)
            return {};
        const size_t value_pos = SkipJsonWhitespace(json, colon + 1, range_end);
        return JsonStringValueAt(json, value_pos, range_end);
    }

    // Brief: ExpandPackageTarget belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string ExpandPackageTarget(const std::string &target, const std::string &subpath)
    {
        std::string expanded = target;
        for (size_t star = expanded.find('*');
             star != std::string::npos;
             star = expanded.find('*', star + subpath.size()))
        {
            expanded.replace(star, 1, subpath);
        }
        return expanded;
    }

    // Brief: TryResolvePackageTarget belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolvePackageTarget(const std::filesystem::path &package_dir,
                                 const std::string &target,
                                 const std::string &subpath,
                                 std::filesystem::path *out)
    {
        const std::string expanded = ExpandPackageTarget(target, subpath);
        return IsRuntimePackageTarget(expanded) && TryResolveAsFile(package_dir / expanded, out);
    }

    // Brief: PackageExportsSearchEnd belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    size_t PackageExportsSearchEnd(const std::string &package_json, size_t key_pos, size_t fallback_size)
    {
        size_t search_end = package_json.find("\n    \"./", key_pos + 1);
        if (search_end == std::string::npos)
            search_end = package_json.find("\n  }", key_pos + 1);
        if (search_end == std::string::npos)
            search_end = std::min(package_json.size(), key_pos + fallback_size);
        return search_end;
    }

    // Brief: TryResolvePackageExportsKey belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolvePackageExportsKey(const std::filesystem::path &package_dir,
                                     const std::string &package_json,
                                     const std::string &key,
                                     const std::string &subpath,
                                     std::filesystem::path *out)
    {
        for (size_t key_pos = package_json.find(key);
             key_pos != std::string::npos;
             key_pos = package_json.find(key, key_pos + key.size()))
        {
            const size_t search_end = PackageExportsSearchEnd(package_json, key_pos, size_t{1000});
            const char *const conditions[] = {"import", "module", "default"};
            for (const char *condition : conditions)
            {
                const std::string target = JsonTargetAfterCondition(package_json, key_pos + key.size(), search_end, condition);
                if (TryResolvePackageTarget(package_dir, target, subpath, out))
                    return true;
            }
            const std::string target = JsonDirectStringValueAfterKey(package_json, key_pos, key.size(), search_end);
            if (TryResolvePackageTarget(package_dir, target, subpath, out))
                return true;
        }
        return false;
    }

    // Brief: TryResolvePackageEntry belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolvePackageEntry(const std::filesystem::path &package_dir, std::filesystem::path *out)
    {
        const std::string package_json = ReadTextFile(package_dir / "package.json");
        if (TryResolvePackageExportsKey(package_dir, package_json, "\".\"", "", out))
            return true;
        std::vector<std::string> candidates;
        auto add_json_string_after = [&](const std::string &key) {
            size_t key_pos = package_json.find("\"" + key + "\"");
            if (key_pos == std::string::npos)
                return;
            size_t colon = package_json.find(':', key_pos);
            size_t quote = package_json.find('"', colon == std::string::npos ? key_pos : colon + 1);
            size_t end = quote == std::string::npos ? std::string::npos : package_json.find('"', quote + 1);
            if (quote != std::string::npos && end != std::string::npos)
                candidates.push_back(package_json.substr(quote + 1, end - quote - 1));
        };
        add_json_string_after("import");
        add_json_string_after(".");
        add_json_string_after("module");
        add_json_string_after("default");
        add_json_string_after("main");
        add_json_string_after("exports");
        candidates.push_back("index.js");
        candidates.push_back("index.mjs");
        for (const std::string &candidate : candidates)
        {
            if (IsRuntimePackageTarget(candidate) && TryResolveAsFile(package_dir / candidate, out))
                return true;
        }
        return false;
    }

    // Brief: TryResolvePackageSubpath belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolvePackageSubpath(const std::filesystem::path &package_dir,
                                  const std::string &subpath,
                                  std::filesystem::path *out)
    {
        if (subpath.empty())
            return TryResolvePackageEntry(package_dir, out);
        const std::string package_json = ReadTextFile(package_dir / "package.json");
        const std::string key = "\"./" + subpath + "\"";
        if (TryResolvePackageExportsKey(package_dir, package_json, key, "", out))
            return true;
        if (TryResolvePackageExportsKey(package_dir, package_json, "\"./*\"", subpath, out))
            return true;
        const std::filesystem::path subpath_dir = package_dir / subpath;
        std::error_code ec;
        if (std::filesystem::is_directory(subpath_dir, ec) && !ec &&
            TryResolvePackageEntry(subpath_dir, out))
        {
            return true;
        }
        return TryResolveAsFile(subpath_dir, out);
    }

    // Brief: TryResolvePackageImport belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool TryResolvePackageImport(const std::filesystem::path &base_dir,
                                 const std::string &specifier,
                                 std::filesystem::path *out)
    {
        for (std::filesystem::path dir = base_dir; !dir.empty(); dir = dir.parent_path())
        {
            const std::filesystem::path package_json_path = dir / "package.json";
            std::error_code ec;
            if (std::filesystem::is_regular_file(package_json_path, ec) && !ec)
            {
                const std::string package_json = ReadTextFile(package_json_path);
                const std::string key = "\"" + specifier + "\"";
                const size_t key_pos = package_json.find(key);
                if (key_pos != std::string::npos)
                {
                    auto target_after = [&](const char *condition) -> std::string {
                        size_t search_end = package_json.find("\n    \"#", key_pos + key.size());
                        if (search_end == std::string::npos)
                            search_end = package_json.find("\n  }", key_pos + key.size());
                        if (search_end == std::string::npos)
                            search_end = std::min(package_json.size(), key_pos + size_t{500});
                        size_t condition_pos = package_json.find(std::string("\"") + condition + "\"", key_pos);
                        if (condition_pos == std::string::npos || condition_pos > search_end)
                            return {};
                        size_t colon = package_json.find(':', condition_pos);
                        size_t quote = package_json.find('"', colon == std::string::npos ? condition_pos : colon + 1);
                        size_t end = quote == std::string::npos ? std::string::npos : package_json.find('"', quote + 1);
                        if (quote == std::string::npos || end == std::string::npos || end > search_end)
                            return {};
                        return package_json.substr(quote + 1, end - quote - 1);
                    };
                    std::string target = target_after("default");
                    if (target.empty())
                        target = target_after("import");
                    if (IsRuntimePackageTarget(target) && TryResolveAsFile(dir / target, out))
                        return true;
                }
            }
            if (dir == dir.root_path())
                break;
        }
        return false;
    }

    // Brief: ResolveModuleSpecifier belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool ResolveModuleSpecifier(const std::string &base, const std::string &specifier, std::string *out)
    {
        if (IsNodeBuiltinSpecifier(specifier))
        {
            *out = specifier;
            return true;
        }
        return edge_quickjs::module_loader::ResolveESMPath(base, specifier, out);
    }

    // Brief: ModuleDefFromValue belongs to the module loading compatibility layer.
    // It unwraps a compiled QuickJS module value into the module definition pointer.
    // Inputs stay as QuickJS handles owned by the caller.
    // Non-module values return null so callers can surface the appropriate N-API error.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSModuleDef *ModuleDefFromValue(JSValueConst value)
    {
        if (JS_VALUE_GET_TAG(value) != JS_TAG_MODULE)
            return nullptr;
        return static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(value));
    }

    // Brief: StoreModuleError belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void StoreModuleError(napi_env env, QuickjsModuleWrap *module)
    {
        if (!CheckEnv(env) || module == nullptr)
            return;
        JSContext *ctx = Ctx(env);
        JSValue error = JS_GetException(ctx);
        if (JS_IsException(error) || JS_IsUndefined(error))
            error = JS_NewError(ctx);
        if (!JS_IsUndefined(module->error))
            JS_FreeValue(ctx, module->error);
        module->error = JS_DupValue(ctx, error);
        module->status = kQuickjsModuleErrored;
        napi_util__::set_last_exception(env, error);
    }

    // Brief: SetModuleImportMetaUrl belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    int SetModuleImportMetaUrl(JSContext *ctx, JSValueConst module_value, const std::string &url)
    {
        JSModuleDef *module = ModuleDefFromValue(module_value);
        if (module == nullptr)
            return -1;
        JSValue meta = JS_GetImportMeta(ctx, module);
        if (JS_IsException(meta))
            return -1;
        const std::string href = PathToFileUrl(url);
        JS_SetPropertyStr(ctx, meta, "url", JS_NewStringLen(ctx, href.c_str(), href.size()));
        JS_FreeValue(ctx, meta);
        return 0;
    }

    // Brief: GetModuleNameString belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::string GetModuleNameString(JSContext *ctx, JSModuleDef *module)
    {
        JSAtom atom = JS_GetModuleName(ctx, module);
        JSValue name_value = JS_AtomToString(ctx, atom);
        JS_FreeAtom(ctx, atom);
        const char *name_cstr = JS_ToCString(ctx, name_value);
        std::string out = name_cstr != nullptr ? std::string(name_cstr) : std::string();
        JS_FreeCString(ctx, name_cstr);
        JS_FreeValue(ctx, name_value);
        return out;
    }

    // Brief: FileLooksCommonJs belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool FileLooksCommonJs(const std::string &path, const std::string &source)
    {
        std::filesystem::path file(path);
        const std::string ext = file.extension().string();
        if (ext == ".cjs")
            return true;
        if (ext == ".mjs")
            return false;
        for (std::filesystem::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path())
        {
            std::error_code ec;
            const std::filesystem::path package_json_path = dir / "package.json";
            if (std::filesystem::is_regular_file(package_json_path, ec) && !ec)
            {
                if (edge_quickjs::module_loader::PackageTypeIsModule(package_json_path))
                    return false;
                break;
            }
            if (dir.filename() == "node_modules" || dir == dir.root_path())
                break;
        }
        return source.find("module.exports") != std::string::npos ||
               source.find("exports.") != std::string::npos ||
               source.find("'use strict'") != std::string::npos;
    }

    // Brief: QuickjsCommonJsModuleInit belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    int QuickjsCommonJsModuleInit(JSContext *ctx, JSModuleDef *module)
    {
        const std::string filename = GetModuleNameString(ctx, module);
        JSValue module_builtin = GetBuiltinModuleValue(ctx, "module");
        JSValue create_require = JS_IsObject(module_builtin) ? JS_GetPropertyStr(ctx, module_builtin, "createRequire")
                                                             : JS_UNDEFINED;
        if (!JS_IsFunction(ctx, create_require))
        {
            JS_FreeValue(ctx, create_require);
            JS_FreeValue(ctx, module_builtin);
            JS_ThrowReferenceError(ctx, "CommonJS module loader unavailable");
            return -1;
        }
        const std::string url = PathToFileUrl(filename);
        JSValue url_value = JS_NewStringLen(ctx, url.c_str(), url.size());
        JSValue require_fn = JS_Call(ctx, create_require, module_builtin, 1, &url_value);
        JS_FreeValue(ctx, url_value);
        JS_FreeValue(ctx, create_require);
        JS_FreeValue(ctx, module_builtin);
        if (JS_IsException(require_fn))
            return -1;

        JSValue filename_value = JS_NewStringLen(ctx, filename.c_str(), filename.size());
        JSValue exports = JS_Call(ctx, require_fn, JS_UNDEFINED, 1, &filename_value);
        JS_FreeValue(ctx, filename_value);
        JS_FreeValue(ctx, require_fn);
        if (JS_IsException(exports))
            return -1;
        if (JS_SetModuleExport(ctx, module, "default", JS_DupValue(ctx, exports)) < 0)
            return -1;
        if (JS_SetModuleExport(ctx, module, "module.exports", JS_DupValue(ctx, exports)) < 0)
            return -1;
        for (const std::string &export_name :
             quickjs_napi::common_js_export_names_for_file(filename, ReadTextFile(filename)))
        {
            JSValue value = JS_GetPropertyStr(ctx, exports, export_name.c_str());
            if (JS_IsException(value))
                return -1;
            if (JS_SetModuleExport(ctx, module, export_name.c_str(), value) < 0)
                return -1;
        }
        JS_FreeValue(ctx, exports);
        return 0;
    }

    // Brief: BuiltinExportNames belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::vector<std::string> BuiltinExportNames(const std::string &specifier)
    {
        std::string name = StartsWith(specifier, "node:") ? specifier.substr(5) : specifier;
        if (name == "util")
            return {"default", "format", "formatWithOptions", "inspect", "promisify", "types"};
        if (name == "path")
            return {"default", "basename", "dirname", "extname", "format", "isAbsolute", "join", "normalize",
                    "parse", "relative", "resolve", "sep", "delimiter"};
        if (name == "fs")
            return {"default", "existsSync", "promises", "readFileSync", "statSync", "writeFileSync"};
        if (name == "url")
            return {"default", "URL", "URLSearchParams", "fileURLToPath", "pathToFileURL"};
        if (name == "module")
            return {"default", "createRequire", "builtinModules", "Module"};
        if (name == "os")
            return {"default", "arch", "cpus", "homedir", "platform", "release", "tmpdir", "type"};
        if (name == "buffer")
            return {"default", "Buffer", "Blob", "File"};
        if (name == "events")
            return {"default", "EventEmitter", "once", "on"};
        if (name == "assert")
            return {"default", "ok", "equal", "deepEqual", "strictEqual", "deepStrictEqual"};
        if (name == "process")
            return {"default"};
        return {"default"};
    }

    // Brief: GetBuiltinModuleValue belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue GetBuiltinModuleValue(JSContext *ctx, const std::string &specifier)
    {
        std::string builtin_name = StartsWith(specifier, "node:") ? specifier.substr(5) : specifier;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue process = JS_GetPropertyStr(ctx, global, "process");
        JS_FreeValue(ctx, global);
        JSValue get_builtin = JS_IsObject(process) ? JS_GetPropertyStr(ctx, process, "getBuiltinModule") : JS_UNDEFINED;
        JSValue builtin_name_value = JS_NewString(ctx, builtin_name.c_str());
        JSValue exports = JS_IsFunction(ctx, get_builtin)
                              ? JS_Call(ctx, get_builtin, process, 1, &builtin_name_value)
                              : JS_UNDEFINED;
        JS_FreeValue(ctx, builtin_name_value);
        JS_FreeValue(ctx, get_builtin);
        JS_FreeValue(ctx, process);
        return exports;
    }

    // Brief: BuiltinExportNames belongs to the module loading compatibility layer.
    // It reflects runtime builtin exports when available and falls back to known names.
    // Inputs stay as QuickJS handles owned by the caller.
    // Missing builtin objects produce a conservative default export set.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    std::vector<std::string> BuiltinExportNames(JSContext *ctx, const std::string &specifier)
    {
        std::vector<std::string> names = {"default"};
        JSValue exports = GetBuiltinModuleValue(ctx, specifier);
        if (!JS_IsObject(exports))
        {
            JS_FreeValue(ctx, exports);
            const std::vector<std::string> fallback = BuiltinExportNames(specifier);
            for (const std::string &name : fallback)
            {
                if (std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
            }
            return names;
        }
        JSPropertyEnum *props = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &len, exports, JS_GPN_STRING_MASK) == 0)
        {
            for (uint32_t i = 0; i < len; ++i)
            {
                const char *name = JS_AtomToCString(ctx, props[i].atom);
                if (name != nullptr && std::find(names.begin(), names.end(), name) == names.end())
                    names.emplace_back(name);
                JS_FreeCString(ctx, name);
            }
            JS_FreePropertyEnum(ctx, props, len);
        }
        JS_FreeValue(ctx, exports);
        return names;
    }

    // Brief: QuickjsBuiltinModuleInit belongs to the module loading compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    int QuickjsBuiltinModuleInit(JSContext *ctx, JSModuleDef *module)
    {
        JSAtom atom = JS_GetModuleName(ctx, module);
        JSValue name_value = JS_AtomToString(ctx, atom);
        JS_FreeAtom(ctx, atom);
        const char *specifier_cstr = JS_ToCString(ctx, name_value);
        std::string specifier = specifier_cstr != nullptr ? std::string(specifier_cstr) : std::string();
        JS_FreeCString(ctx, specifier_cstr);
        JS_FreeValue(ctx, name_value);
        JSValue exports = GetBuiltinModuleValue(ctx, specifier);
        if (JS_IsException(exports))
            return -1;

        for (const std::string &name : BuiltinExportNames(ctx, specifier))
        {
            JSValue value = name == "default" ? JS_DupValue(ctx, exports) : JS_GetPropertyStr(ctx, exports, name.c_str());
            if (JS_IsException(value))
            {
                JS_FreeValue(ctx, exports);
                return -1;
            }
            if (JS_SetModuleExport(ctx, module, name.c_str(), value) < 0)
            {
                JS_FreeValue(ctx, exports);
                return -1;
            }
        }
        JS_FreeValue(ctx, exports);
        return 0;
    }

    // Brief: QuickjsModuleNormalize belongs to the module loading compatibility layer.
    // It bridges QuickJS module specifier normalization to Edge's Node-like resolver.
    // Inputs stay as QuickJS handles and borrowed C strings owned by the caller.
    // Misses intentionally return the original specifier so QuickJS can report the load failure.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    char *QuickjsModuleNormalize(JSContext *ctx, const char *module_base_name, const char *module_name, void *opaque)
    {
        (void)opaque;
        std::string resolved;
        if (!ResolveModuleSpecifier(module_base_name != nullptr ? module_base_name : "",
                                    module_name != nullptr ? module_name : "",
                                    &resolved))
        {
            if (EDGE_TRACE_ENABLED("EDGE_TRACE_QUICKJS_MODULES"))
                std::fprintf(stderr, "quickjs-module normalize-miss base=%s spec=%s\n",
                             module_base_name != nullptr ? module_base_name : "",
                             module_name != nullptr ? module_name : "");
            return DupCString(ctx, module_name != nullptr ? std::string(module_name) : std::string());
        }
        if (EDGE_TRACE_ENABLED("EDGE_TRACE_QUICKJS_MODULES"))
            std::fprintf(stderr, "quickjs-module normalize base=%s spec=%s -> %s\n",
                         module_base_name != nullptr ? module_base_name : "",
                         module_name != nullptr ? module_name : "",
                         resolved.c_str());
        return DupCString(ctx, resolved);
    }

    // Brief: QuickjsModuleLoader belongs to the module loading compatibility layer.
    // It loads builtins, CommonJS facades, and ESM source for QuickJS module execution.
    // Inputs stay as QuickJS handles and borrowed C strings owned by the caller.
    // Failures throw QuickJS module-load exceptions for the engine to propagate.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSModuleDef *QuickjsModuleLoader(JSContext *ctx, const char *module_name, void *opaque)
    {
        (void)opaque;
        std::string name = module_name != nullptr ? std::string(module_name) : std::string();
        if (IsNodeBuiltinSpecifier(name))
        {
            JSModuleDef *module = JS_NewCModule(ctx, name.c_str(), QuickjsBuiltinModuleInit);
            if (module == nullptr)
                return nullptr;
            for (const std::string &export_name : BuiltinExportNames(ctx, name))
            {
                if (JS_AddModuleExport(ctx, module, export_name.c_str()) < 0)
                    return nullptr;
            }
            return module;
        }

        std::string source = ReadTextFile(name);
        if (source.empty())
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(name, ec) || ec)
            {
                JS_ThrowReferenceError(ctx, "could not load module '%s'", name.c_str());
                return nullptr;
            }
        }
        if (FileLooksCommonJs(name, source))
        {
            JSModuleDef *module = JS_NewCModule(ctx, name.c_str(), QuickjsCommonJsModuleInit);
            if (module == nullptr)
                return nullptr;
            if (JS_AddModuleExport(ctx, module, "default") < 0)
                return nullptr;
            if (JS_AddModuleExport(ctx, module, "module.exports") < 0)
                return nullptr;
            for (const std::string &export_name :
                 quickjs_napi::common_js_export_names_for_file(name, source))
            {
                if (JS_AddModuleExport(ctx, module, export_name.c_str()) < 0)
                    return nullptr;
            }
            return module;
        }
        JSValue compiled = JS_Eval(ctx,
                                   source.c_str(),
                                   source.size(),
                                   name.c_str(),
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled))
            return nullptr;
        JSModuleDef *module = ModuleDefFromValue(compiled);
        if (module != nullptr && SetModuleImportMetaUrl(ctx, compiled, name) < 0)
        {
            JS_FreeValue(ctx, compiled);
            return nullptr;
        }
        JS_FreeValue(ctx, compiled);
        return module;
    }
}
