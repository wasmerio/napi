#include "unofficial_node_compat.h"

#include "internal/napi_env.h"
#include "internal/napi_util.h"
#include "internal/quickjs_trace.h"
#include "node_api.h"
#include "quickjs_cjs_exports.h"
#include "unofficial_module_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>

namespace quickjs::detail
{
    struct SerdesSerializer
    {
        std::vector<uint8_t> bytes;
    };

    struct SerdesDeserializer
    {
        std::vector<uint8_t> bytes;
        size_t offset = 0;
    };

    std::mutex g_mu;
    EmbedderHooksState g_embedder_hooks;
    std::unordered_map<napi_env, EnvState> g_env_states;

    EnvState &EnsureEnvState(napi_env env);
    JSValue GetBuiltinModuleValue(JSContext *ctx, const std::string &specifier);

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

    napi_value UndefinedValue(napi_env env)
    {
        napi_value out = nullptr;
        napi_get_undefined(env, &out);
        return out;
    }

    bool StartsWith(const std::string &value, const char *prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

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

    std::string JsonStringValueAt(const std::string &json, size_t quote_pos, size_t limit)
    {
        if (quote_pos >= limit || quote_pos >= json.size() || json[quote_pos] != '"')
            return {};
        const size_t end = FindJsonStringEnd(json, quote_pos, limit);
        if (end == std::string::npos)
            return {};
        return json.substr(quote_pos + 1, end - quote_pos - 1);
    }

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

    bool TryResolvePackageTarget(const std::filesystem::path &package_dir,
                                 const std::string &target,
                                 const std::string &subpath,
                                 std::filesystem::path *out)
    {
        const std::string expanded = ExpandPackageTarget(target, subpath);
        return IsRuntimePackageTarget(expanded) && TryResolveAsFile(package_dir / expanded, out);
    }

    size_t PackageExportsSearchEnd(const std::string &package_json, size_t key_pos, size_t fallback_size)
    {
        size_t search_end = package_json.find("\n    \"./", key_pos + 1);
        if (search_end == std::string::npos)
            search_end = package_json.find("\n  }", key_pos + 1);
        if (search_end == std::string::npos)
            search_end = std::min(package_json.size(), key_pos + fallback_size);
        return search_end;
    }

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

    bool ResolveModuleSpecifier(const std::string &base, const std::string &specifier, std::string *out)
    {
        if (IsNodeBuiltinSpecifier(specifier))
        {
            *out = specifier;
            return true;
        }
        return edge_quickjs::module_loader::ResolveESMPath(base, specifier, out);
    }

    char *DupCString(JSContext *ctx, const std::string &value)
    {
        char *out = static_cast<char *>(js_malloc(ctx, value.size() + 1));
        if (out == nullptr)
            return nullptr;
        std::memcpy(out, value.c_str(), value.size() + 1);
        return out;
    }

    JSModuleDef *ModuleDefFromValue(JSValueConst value)
    {
        if (JS_VALUE_GET_TAG(value) != JS_TAG_MODULE)
            return nullptr;
        return static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(value));
    }

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
        return EDGE_TRACE_ENABLED("EDGE_TRACE_QUICKJS_CONTEXTIFY") ||
               EDGE_TRACE_ENABLED("EDGE_TRACE_BUILTINS");
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
            // JS_FreeRuntime(scope->rt);
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

    bool ReadBytesFromArrayBufferLike(napi_env env,
                                      napi_value value,
                                      std::vector<uint8_t> *bytes_out)
    {
        if (value == nullptr || bytes_out == nullptr)
            return false;

        JSContext *ctx = Ctx(env);
        JSValueConst input = value->get_inner();
        uint8_t *data = nullptr;
        size_t length = 0;

        if (JS_IsArrayBuffer(input))
        {
            data = JS_GetArrayBuffer(ctx, &length, input);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data, data + length);
            return true;
        }

        if (JS_GetTypedArrayType(input) >= 0)
        {
            size_t offset = 0;
            JSValue array_buffer = JS_GetTypedArrayBuffer(ctx, input, &offset, &length, nullptr);
            if (JS_IsException(array_buffer))
                return false;
            size_t array_buffer_length = 0;
            data = JS_GetArrayBuffer(ctx, &array_buffer_length, array_buffer);
            JS_FreeValue(ctx, array_buffer);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (offset > array_buffer_length || length > array_buffer_length - offset)
                return false;
            if (length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data + offset, data + offset + length);
            return true;
        }

        if (JS_IsDataView(input))
        {
            JSValue buffer = JS_GetPropertyStr(ctx, input, "buffer");
            JSValue byte_offset = JS_GetPropertyStr(ctx, input, "byteOffset");
            JSValue byte_length = JS_GetPropertyStr(ctx, input, "byteLength");
            uint32_t offset = 0;
            uint32_t view_length = 0;
            bool ok = !JS_IsException(buffer) &&
                      JS_ToUint32(ctx, &offset, byte_offset) == 0 &&
                      JS_ToUint32(ctx, &view_length, byte_length) == 0;
            JS_FreeValue(ctx, byte_offset);
            JS_FreeValue(ctx, byte_length);
            if (!ok)
            {
                JS_FreeValue(ctx, buffer);
                return false;
            }
            size_t array_buffer_length = 0;
            data = JS_GetArrayBuffer(ctx, &array_buffer_length, buffer);
            JS_FreeValue(ctx, buffer);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (offset > array_buffer_length || view_length > array_buffer_length - offset)
                return false;
            if (view_length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data + offset, data + offset + view_length);
            return true;
        }

        return false;
    }

    template <typename T>
    void AppendLittleEndian(std::vector<uint8_t> *bytes, T value)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
            bytes->push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (i * 8)) & 0xff));
    }

    bool ReadLittleEndian(const std::vector<uint8_t> &bytes,
                          size_t *offset,
                          size_t width,
                          uint64_t *value_out)
    {
        if (offset == nullptr || value_out == nullptr || *offset > bytes.size() ||
            width > bytes.size() - *offset || width > sizeof(uint64_t))
            return false;
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= static_cast<uint64_t>(bytes[*offset + i]) << (i * 8);
        *offset += width;
        *value_out = value;
        return true;
    }

    void SerdesSerializerFinalize(napi_env /*env*/, void *data, void * /*hint*/)
    {
        delete static_cast<SerdesSerializer *>(data);
    }

    void SerdesDeserializerFinalize(napi_env /*env*/, void *data, void * /*hint*/)
    {
        delete static_cast<SerdesDeserializer *>(data);
    }

    SerdesSerializer *GetSerdesSerializer(napi_env env, napi_value this_arg)
    {
        void *data = nullptr;
        if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr)
            return nullptr;
        return static_cast<SerdesSerializer *>(data);
    }

    SerdesDeserializer *GetSerdesDeserializer(napi_env env, napi_value this_arg)
    {
        void *data = nullptr;
        if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr)
            return nullptr;
        return static_cast<SerdesDeserializer *>(data);
    }

    napi_value SerdesSerializerNew(napi_env env, napi_callback_info info)
    {
        napi_value new_target = nullptr;
        if (napi_get_new_target(env, info, &new_target) != napi_ok || new_target == nullptr)
        {
            napi_throw_type_error(env,
                                  "ERR_CONSTRUCT_CALL_REQUIRED",
                                  "Class constructor Serializer cannot be invoked without 'new'");
            return nullptr;
        }

        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr)
            return nullptr;

        auto *serializer = new (std::nothrow) SerdesSerializer();
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Failed to allocate Serializer");
            return nullptr;
        }
        if (napi_wrap(env, this_arg, serializer, SerdesSerializerFinalize, nullptr, nullptr) != napi_ok)
        {
            delete serializer;
            napi_throw_error(env, nullptr, "Failed to initialize Serializer");
            return nullptr;
        }
        return nullptr;
    }

    napi_value SerdesSerializerWriteHeader(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerWriteValue(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;

        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }

        napi_value value = argc >= 1 && argv[0] != nullptr ? argv[0] : UndefinedValue(env);
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
        {
            if (!JS_HasException(Ctx(env)))
                napi_throw_error(env, nullptr, "Value could not be serialized");
            return nullptr;
        }

        serializer->bytes.insert(serializer->bytes.end(), bytes, bytes + size);
        js_free(Ctx(env), bytes);

        napi_value result = nullptr;
        napi_get_boolean(env, true, &result);
        return result;
    }

    napi_value SerdesSerializerReleaseBuffer(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }

        napi_value buffer = nullptr;
        const void *data = serializer->bytes.empty() ? nullptr : serializer->bytes.data();
        if (napi_create_buffer_copy(env, serializer->bytes.size(), data, nullptr, &buffer) != napi_ok)
            return nullptr;
        serializer->bytes.clear();
        return buffer;
    }

    napi_value SerdesSerializerTransferArrayBuffer(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerWriteUint32(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        uint32_t value = 0;
        if (argc < 1 || napi_get_value_uint32(env, argv[0], &value) != napi_ok)
            return UndefinedValue(env);
        AppendLittleEndian<uint32_t>(&serializer->bytes, value);
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerWriteUint64(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[2] = {nullptr, nullptr};
        size_t argc = 2;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        uint32_t hi = 0;
        uint32_t lo = 0;
        if (argc < 2 || napi_get_value_uint32(env, argv[0], &hi) != napi_ok ||
            napi_get_value_uint32(env, argv[1], &lo) != napi_ok)
            return UndefinedValue(env);
        AppendLittleEndian<uint64_t>(&serializer->bytes,
                                     (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo));
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerWriteDouble(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        double value = 0;
        if (argc < 1 || napi_get_value_double(env, argv[0], &value) != napi_ok)
            return UndefinedValue(env);
        const auto *raw = reinterpret_cast<const uint8_t *>(&value);
        serializer->bytes.insert(serializer->bytes.end(), raw, raw + sizeof(value));
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerWriteRawBytes(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        std::vector<uint8_t> bytes;
        if (argc < 1 || !ReadBytesFromArrayBufferLike(env, argv[0], &bytes))
        {
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "source must be a TypedArray or a DataView");
            return nullptr;
        }
        serializer->bytes.insert(serializer->bytes.end(), bytes.begin(), bytes.end());
        return UndefinedValue(env);
    }

    napi_value SerdesSerializerSetTreatArrayBufferViewsAsHostObjects(napi_env env,
                                                                     napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    napi_value SerdesDeserializerNew(napi_env env, napi_callback_info info)
    {
        napi_value new_target = nullptr;
        if (napi_get_new_target(env, info, &new_target) != napi_ok || new_target == nullptr)
        {
            napi_throw_type_error(env,
                                  "ERR_CONSTRUCT_CALL_REQUIRED",
                                  "Class constructor Deserializer cannot be invoked without 'new'");
            return nullptr;
        }

        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr)
            return nullptr;
        if (argc < 1 || argv[0] == nullptr)
        {
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "buffer must be a TypedArray or a DataView");
            return nullptr;
        }

        auto *deserializer = new (std::nothrow) SerdesDeserializer();
        if (deserializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Failed to allocate Deserializer");
            return nullptr;
        }
        if (!ReadBytesFromArrayBufferLike(env, argv[0], &deserializer->bytes))
        {
            delete deserializer;
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "buffer must be a TypedArray or a DataView");
            return nullptr;
        }
        if (napi_wrap(env, this_arg, deserializer, SerdesDeserializerFinalize, nullptr, nullptr) != napi_ok)
        {
            delete deserializer;
            napi_throw_error(env, nullptr, "Failed to initialize Deserializer");
            return nullptr;
        }
        napi_set_named_property(env, this_arg, "buffer", argv[0]);
        return nullptr;
    }

    napi_value SerdesDeserializerReadHeader(napi_env env, napi_callback_info /*info*/)
    {
        napi_value result = nullptr;
        napi_get_boolean(env, true, &result);
        return result;
    }

    napi_value SerdesDeserializerReadValue(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        if (deserializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Deserializer state");
            return nullptr;
        }
        if (deserializer->offset > deserializer->bytes.size())
        {
            napi_throw_error(env, nullptr, "Deserializer offset is out of range");
            return nullptr;
        }

        JSValue value = JS_ReadObject(Ctx(env),
                                      deserializer->bytes.data() + deserializer->offset,
                                      deserializer->bytes.size() - deserializer->offset,
                                      JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        if (JS_IsException(value))
            return nullptr;
        deserializer->offset = deserializer->bytes.size();

        napi_value result = nullptr;
        if (WrapOwned(env, value, &result) != napi_ok)
            return nullptr;
        return result;
    }

    napi_value SerdesDeserializerGetWireFormatVersion(napi_env env, napi_callback_info /*info*/)
    {
        napi_value result = nullptr;
        napi_create_uint32(env, 0, &result);
        return result;
    }

    napi_value SerdesDeserializerTransferArrayBuffer(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    napi_value SerdesDeserializerReadUint32(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        uint64_t value = 0;
        if (deserializer == nullptr || !ReadLittleEndian(deserializer->bytes, &deserializer->offset, 4, &value))
        {
            napi_throw_error(env, nullptr, "ReadUint32() failed");
            return nullptr;
        }
        napi_value result = nullptr;
        napi_create_uint32(env, static_cast<uint32_t>(value), &result);
        return result;
    }

    napi_value SerdesDeserializerReadUint64(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        uint64_t value = 0;
        if (deserializer == nullptr || !ReadLittleEndian(deserializer->bytes, &deserializer->offset, 8, &value))
        {
            napi_throw_error(env, nullptr, "ReadUint64() failed");
            return nullptr;
        }

        napi_value result = nullptr;
        napi_value hi = nullptr;
        napi_value lo = nullptr;
        napi_create_array_with_length(env, 2, &result);
        napi_create_uint32(env, static_cast<uint32_t>(value >> 32), &hi);
        napi_create_uint32(env, static_cast<uint32_t>(value), &lo);
        napi_set_element(env, result, 0, hi);
        napi_set_element(env, result, 1, lo);
        return result;
    }

    napi_value SerdesDeserializerReadDouble(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        if (deserializer == nullptr ||
            deserializer->offset > deserializer->bytes.size() ||
            sizeof(double) > deserializer->bytes.size() - deserializer->offset)
        {
            napi_throw_error(env, nullptr, "ReadDouble() failed");
            return nullptr;
        }
        double value = 0;
        std::memcpy(&value, deserializer->bytes.data() + deserializer->offset, sizeof(value));
        deserializer->offset += sizeof(value);
        napi_value result = nullptr;
        napi_create_double(env, value, &result);
        return result;
    }

    napi_value SerdesDeserializerReadRawBytes(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        int64_t length = 0;
        if (deserializer == nullptr || argc < 1 || napi_get_value_int64(env, argv[0], &length) != napi_ok ||
            length < 0 || deserializer->offset > deserializer->bytes.size() ||
            static_cast<size_t>(length) > deserializer->bytes.size() - deserializer->offset)
        {
            napi_throw_error(env, nullptr, "ReadRawBytes() failed");
            return nullptr;
        }

        size_t offset = deserializer->offset;
        deserializer->offset += static_cast<size_t>(length);
        napi_value result = nullptr;
        napi_create_uint32(env, static_cast<uint32_t>(offset), &result);
        return result;
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

    // Undici is Node's fetch/HTTP client. It loads llhttp, its HTTP/1 parser,
    // through a small WebAssembly module; QuickJS does not expose a general
    // WebAssembly engine here, so provide only the parser-shaped surface Undici
    // asks for.
    void EnsureUndiciLlhttpWebAssemblyShim(JSContext *ctx)
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue existing = JS_GetPropertyStr(ctx, global, "WebAssembly");
        const bool missing = !JS_IsException(existing) && JS_IsUndefined(existing);
        JS_FreeValue(ctx, existing);
        JS_FreeValue(ctx, global);
        if (!missing)
            return;

        static constexpr const char *kShim = R"JS(
(() => {
  "use strict";
  const OK = 0;
  const PAUSED = 21;
  const PAUSED_UPGRADE = 22;
  const TYPE_RESPONSE = 2;
  const textDecoder = typeof TextDecoder === "function" ? new TextDecoder("latin1") : null;
  let memoryBuffer = new ArrayBuffer(1024 * 1024);
  let heapTop = 1024;
  let nextParser = 1;
  let env = null;
  const parsers = new Map();

  function ensureMemory(size) {
    if (size <= memoryBuffer.byteLength) return;
    let next = memoryBuffer.byteLength;
    while (next < size) next *= 2;
    const grown = new ArrayBuffer(next);
    new Uint8Array(grown).set(new Uint8Array(memoryBuffer));
    memoryBuffer = grown;
  }

  function decode(bytes) {
    if (textDecoder) return textDecoder.decode(bytes);
    let out = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      out += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    return out;
  }

  function callback(name, ...args) {
    const fn = env && env[name];
    return typeof fn === "function" ? fn(...args) : OK;
  }

  function resetParser(p, type = p.type) {
    p.type = type;
    p.headerText = "";
    p.state = "headers";
    p.contentLength = null;
    p.bodyRemaining = 0;
    p.chunked = false;
    p.chunkBuffer = "";
    p.statusCode = 0;
    p.shouldKeepAlive = true;
    p.upgrade = false;
    p.errorPos = 0;
    p.errorReason = 0;
  }

  function findHeaderOffsets(chunkText, headerText, lines) {
    const offsets = [];
    let searchFrom = 0;
    for (const line of lines) {
      const index = chunkText.indexOf(line, searchFrom);
      offsets.push(index);
      if (index >= 0) searchFrom = index + line.length + 2;
    }
    return offsets;
  }

  function parseHeaders(p, chunkText, basePtr) {
    const end = p.headerText.indexOf("\r\n\r\n");
    if (end < 0) return false;

    const head = p.headerText.slice(0, end);
    const lines = head.split("\r\n");
    const first = lines.shift() || "";
    callback("wasm_on_message_begin", p.ptr);

    if (p.type === TYPE_RESPONSE) {
      const match = /^HTTP\/(\d+)\.(\d+)\s+(\d+)\s*(.*)$/.exec(first);
      if (match) {
        p.statusCode = Number(match[3]) || 0;
        const statusText = match[4] || "";
        if (statusText) {
          const statusAt = chunkText.indexOf(statusText);
          if (statusAt >= 0) callback("wasm_on_status", p.ptr, basePtr + statusAt, statusText.length);
        }
      }
    }

    const headerOffsets = findHeaderOffsets(chunkText, head, lines);
    const headers = Object.create(null);
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const colon = line.indexOf(":");
      if (colon <= 0) continue;
      const name = line.slice(0, colon);
      const rawValue = line.slice(colon + 1);
      const leading = rawValue.match(/^\s*/)[0].length;
      const value = rawValue.slice(leading);
      const lineAt = headerOffsets[i];
      if (lineAt >= 0) {
        callback("wasm_on_header_field", p.ptr, basePtr + lineAt, name.length);
        callback("wasm_on_header_value", p.ptr, basePtr + lineAt + colon + 1 + leading, value.length);
      }
      headers[name.toLowerCase()] = value;
    }

    const connection = (headers.connection || "").toLowerCase();
    const transferEncoding = (headers["transfer-encoding"] || "").toLowerCase();
    p.contentLength = headers["content-length"] != null ? Number(headers["content-length"]) : null;
    p.chunked = transferEncoding.split(",").map((v) => v.trim()).includes("chunked");
    p.shouldKeepAlive = connection !== "close";
    p.upgrade = connection.includes("upgrade");

    const complete = callback(
      "wasm_on_headers_complete",
      p.ptr,
      p.statusCode,
      p.upgrade ? 1 : 0,
      p.shouldKeepAlive ? 1 : 0
    );
    p.state = "body";
    p.headerText = "";
    if (complete === 1 || p.statusCode === 204 || p.statusCode === 304) {
      callback("wasm_on_message_complete", p.ptr);
      resetParser(p);
      return true;
    }
    if (complete === 2) return PAUSED_UPGRADE;
    if (complete === PAUSED) return PAUSED;
    if (Number.isFinite(p.contentLength)) p.bodyRemaining = Math.max(0, p.contentLength);
    return true;
  }

  function consumeBody(p, chunkText, basePtr, start, end) {
    if (p.chunked) {
      let cursor = start;
      while (cursor < end) {
        const lineEnd = chunkText.indexOf("\r\n", cursor);
        if (lineEnd < 0) break;
        const size = Number.parseInt(chunkText.slice(cursor, lineEnd).split(";", 1)[0], 16);
        if (!Number.isFinite(size)) break;
        const bodyStart = lineEnd + 2;
        const bodyEnd = bodyStart + size;
        if (bodyEnd + 2 > end) break;
        if (size === 0) {
          callback("wasm_on_message_complete", p.ptr);
          resetParser(p);
          return OK;
        }
        const ret = callback("wasm_on_body", p.ptr, basePtr + bodyStart, size);
        if (ret !== OK) return ret;
        cursor = bodyEnd + 2;
      }
      return OK;
    }

    if (Number.isFinite(p.bodyRemaining)) {
      const len = Math.min(p.bodyRemaining, end - start);
      if (len > 0) {
        const ret = callback("wasm_on_body", p.ptr, basePtr + start, len);
        if (ret !== OK) return ret;
        p.bodyRemaining -= len;
      }
      if (p.bodyRemaining === 0) {
        callback("wasm_on_message_complete", p.ptr);
        resetParser(p);
      }
    } else if (end > start) {
      const ret = callback("wasm_on_body", p.ptr, basePtr + start, end - start);
      if (ret !== OK) return ret;
    }
    return OK;
  }

  const exports = {
    get memory() {
      return { buffer: memoryBuffer };
    },
    malloc(size) {
      const ptr = (heapTop + 7) & ~7;
      heapTop = ptr + Math.max(0, size | 0);
      ensureMemory(heapTop + 8);
      return ptr;
    },
    free() {},
    llhttp_alloc(type) {
      const ptr = nextParser++;
      const parser = { ptr };
      resetParser(parser, type);
      parsers.set(ptr, parser);
      return ptr;
    },
    llhttp_free(ptr) {
      parsers.delete(ptr);
    },
    llhttp_init(ptr, type) {
      const parser = parsers.get(ptr);
      if (parser) resetParser(parser, type);
    },
    _initialize() {},
    __indirect_function_table: {},
    llhttp_execute(ptr, dataPtr, len) {
      const parser = parsers.get(ptr);
      if (!parser) return 1;
      const chunkText = decode(new Uint8Array(memoryBuffer, dataPtr, len));
      let bodyStart = 0;
      if (parser.state === "headers") {
        parser.headerText += chunkText;
        const result = parseHeaders(parser, chunkText, dataPtr);
        if (result !== true) return result === false ? OK : result;
        const headerEnd = chunkText.indexOf("\r\n\r\n");
        bodyStart = headerEnd >= 0 ? headerEnd + 4 : len;
      }
      return consumeBody(parser, chunkText, dataPtr, bodyStart, len);
    },
    llhttp_should_keep_alive(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.shouldKeepAlive ? 1 : 0;
    },
    llhttp_get_type(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.type : 0;
    },
    llhttp_get_http_major() { return 1; },
    llhttp_get_http_minor() { return 1; },
    llhttp_get_method() { return 0; },
    llhttp_get_status_code(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.statusCode : 0;
    },
    llhttp_get_upgrade(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.upgrade ? 1 : 0;
    },
    llhttp_reset(ptr) {
      const parser = parsers.get(ptr);
      if (parser) resetParser(parser);
    },
    llhttp_finish(ptr) {
      const parser = parsers.get(ptr);
      if (parser && parser.state === "body" && !Number.isFinite(parser.bodyRemaining)) {
        callback("wasm_on_message_complete", ptr);
        resetParser(parser);
      }
      return OK;
    },
    llhttp_pause() {},
    llhttp_resume() {},
    llhttp_resume_after_upgrade() {},
    llhttp_get_errno() { return OK; },
    llhttp_get_error_reason(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.errorReason : 0;
    },
    llhttp_set_error_reason() {},
    llhttp_get_error_pos(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.errorPos ? parser.errorPos : 0;
    },
    llhttp_errno_name() { return 0; },
    llhttp_method_name() { return 0; },
    llhttp_status_name() { return 0; },
    llhttp_set_lenient_headers() {},
    llhttp_set_lenient_chunked_length() {},
    llhttp_set_lenient_keep_alive() {},
    llhttp_set_lenient_transfer_encoding() {},
    llhttp_set_lenient_version() {},
    llhttp_set_lenient_data_after_close() {},
    llhttp_set_lenient_optional_lf_after_cr() {},
    llhttp_set_lenient_optional_crlf_after_chunk() {},
    llhttp_set_lenient_optional_cr_before_lf() {},
    llhttp_set_lenient_spaces_after_chunk_size() {}
  };

  function Module(bytes) {
    if (!(this instanceof Module)) throw new TypeError("WebAssembly.Module must be called with new");
    this.bytes = bytes;
  }

  function Instance(module, imports) {
    if (!(this instanceof Instance)) throw new TypeError("WebAssembly.Instance must be called with new");
    env = imports && imports.env || {};
    this.exports = exports;
  }

  Object.defineProperty(globalThis, "WebAssembly", {
    configurable: true,
    writable: true,
    value: { Module, Instance }
  });
})();
)JS";

        JSValue result = JS_Eval(ctx, kShim, std::strlen(kShim), "<quickjs-undici-llhttp-wasm-shim>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, result);
    }

    void EnsureQuickjsWeakRefCompat(JSContext *ctx)
    {
        static constexpr const char *kShim = R"JS(
(() => {
  "use strict";
  let needsCompat = false;
  try {
    needsCompat = typeof WeakRef !== "function" || new WeakRef({}).deref() === undefined;
  } catch {
    needsCompat = true;
  }
  if (!needsCompat) return;

  class QuickjsWeakRefCompat {
    constructor(target) {
      if ((typeof target !== "object" && typeof target !== "function") || target === null) {
        throw new TypeError("WeakRef target must be an object");
      }
      this._target = target;
    }

    deref() {
      return this._target;
    }
  }

  Object.defineProperty(QuickjsWeakRefCompat, "name", {
    configurable: true,
    value: "WeakRef"
  });
  Object.defineProperty(globalThis, "WeakRef", {
    configurable: true,
    writable: true,
    value: QuickjsWeakRefCompat
  });
})();
)JS";

        JSValue result = JS_Eval(ctx, kShim, std::strlen(kShim), "<quickjs-weakref-compat>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, result);
    }

    void EnsureQuickjsGlobalCompat(JSContext *ctx)
    {
        EnsureNodeWellKnownSymbols(ctx);
        EnsureQuickjsWeakRefCompat(ctx);
        EnsureUndiciLlhttpWebAssemblyShim(ctx);
    }
}
