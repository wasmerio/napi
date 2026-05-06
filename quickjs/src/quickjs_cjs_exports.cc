#include "quickjs_cjs_exports.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace quickjs_napi
{
namespace
{
    constexpr size_t kMaxReexportDepth = 32;

    bool starts_with(const std::string &value, const char *prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    std::string read_text_file(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void add_export_name(std::vector<std::string> *names, std::string name)
    {
        if (names == nullptr || name.empty() || name == "default" || name == "module.exports")
            return;
        if (std::find(names->begin(), names->end(), name) == names->end())
            names->push_back(std::move(name));
    }

    bool is_identifier_start(char ch)
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '$';
    }

    bool is_identifier_part(char ch)
    {
        return is_identifier_start(ch) || (ch >= '0' && ch <= '9');
    }

    void skip_whitespace(const std::string &source, size_t *pos)
    {
        while (pos != nullptr && *pos < source.size() &&
               (source[*pos] == ' ' || source[*pos] == '\t' ||
                source[*pos] == '\r' || source[*pos] == '\n'))
        {
            ++(*pos);
        }
    }

    bool is_word_boundary(const std::string &source, size_t pos)
    {
        return pos >= source.size() || !is_identifier_part(source[pos]);
    }

    void extract_dot_exports(const std::string &source,
                             const std::string &prefix,
                             std::vector<std::string> *names)
    {
        size_t pos = 0;
        while ((pos = source.find(prefix, pos)) != std::string::npos)
        {
            size_t name_start = pos + prefix.size();
            if (name_start >= source.size() || !is_identifier_start(source[name_start]))
            {
                pos = name_start;
                continue;
            }
            size_t name_end = name_start + 1;
            while (name_end < source.size() && is_identifier_part(source[name_end]))
                ++name_end;
            size_t after_name = name_end;
            skip_whitespace(source, &after_name);
            if (after_name < source.size() && source[after_name] == '=')
                add_export_name(names, source.substr(name_start, name_end - name_start));
            pos = name_end;
        }
    }

    void extract_bracket_exports(const std::string &source,
                                 const std::string &prefix,
                                 std::vector<std::string> *names)
    {
        size_t pos = 0;
        while ((pos = source.find(prefix, pos)) != std::string::npos)
        {
            size_t quote_pos = pos + prefix.size();
            skip_whitespace(source, &quote_pos);
            if (quote_pos >= source.size() || (source[quote_pos] != '"' && source[quote_pos] != '\''))
            {
                pos = quote_pos;
                continue;
            }
            const char quote = source[quote_pos];
            size_t name_start = quote_pos + 1;
            size_t name_end = source.find(quote, name_start);
            if (name_end == std::string::npos)
                break;
            size_t close_pos = name_end + 1;
            skip_whitespace(source, &close_pos);
            if (close_pos < source.size() && source[close_pos] == ']')
            {
                size_t after_bracket = close_pos + 1;
                skip_whitespace(source, &after_bracket);
                if (after_bracket < source.size() && source[after_bracket] == '=')
                    add_export_name(names, source.substr(name_start, name_end - name_start));
            }
            pos = name_end + 1;
        }
    }

    void extract_define_property_exports(const std::string &source,
                                         const std::string &prefix,
                                         std::vector<std::string> *names)
    {
        size_t pos = 0;
        while ((pos = source.find(prefix, pos)) != std::string::npos)
        {
            size_t quote_pos = pos + prefix.size();
            skip_whitespace(source, &quote_pos);
            if (quote_pos >= source.size() || (source[quote_pos] != '"' && source[quote_pos] != '\''))
            {
                pos = quote_pos;
                continue;
            }
            const char quote = source[quote_pos];
            size_t name_start = quote_pos + 1;
            size_t name_end = source.find(quote, name_start);
            if (name_end == std::string::npos)
                break;
            add_export_name(names, source.substr(name_start, name_end - name_start));
            pos = name_end + 1;
        }
    }

    void extract_object_literal_exports(const std::string &source,
                                        std::vector<std::string> *names)
    {
        size_t pos = 0;
        while ((pos = source.find("module.exports", pos)) != std::string::npos)
        {
            size_t cursor = pos + std::strlen("module.exports");
            skip_whitespace(source, &cursor);
            if (cursor >= source.size() || source[cursor] != '=')
            {
                pos = cursor;
                continue;
            }
            ++cursor;
            skip_whitespace(source, &cursor);
            if (cursor >= source.size() || source[cursor] != '{')
            {
                pos = cursor;
                continue;
            }
            ++cursor;
            int depth = 1;
            while (cursor < source.size() && depth > 0)
            {
                skip_whitespace(source, &cursor);
                if (cursor >= source.size())
                    break;
                if (source[cursor] == '{')
                {
                    ++depth;
                    ++cursor;
                    continue;
                }
                if (source[cursor] == '}')
                {
                    --depth;
                    ++cursor;
                    continue;
                }
                if (depth == 1 && source[cursor] == '.')
                {
                    cursor = source.find(',', cursor);
                    if (cursor == std::string::npos)
                        break;
                    ++cursor;
                    continue;
                }
                if (depth == 1 && (is_identifier_start(source[cursor]) || source[cursor] == '"' || source[cursor] == '\''))
                {
                    std::string name;
                    if (source[cursor] == '"' || source[cursor] == '\'')
                    {
                        const char quote = source[cursor++];
                        size_t end = source.find(quote, cursor);
                        if (end == std::string::npos)
                            break;
                        name = source.substr(cursor, end - cursor);
                        cursor = end + 1;
                    }
                    else
                    {
                        size_t start = cursor++;
                        while (cursor < source.size() && is_identifier_part(source[cursor]))
                            ++cursor;
                        name = source.substr(start, cursor - start);
                    }
                    size_t after_name = cursor;
                    skip_whitespace(source, &after_name);
                    if (after_name < source.size() &&
                        (source[after_name] == ':' || source[after_name] == ',' || source[after_name] == '}'))
                    {
                        add_export_name(names, std::move(name));
                    }
                    cursor = after_name;
                    continue;
                }
                ++cursor;
            }
            pos = cursor;
        }
    }

    void extract_direct_export_names(const std::string &source, std::vector<std::string> *names)
    {
        extract_dot_exports(source, "exports.", names);
        extract_dot_exports(source, "module.exports.", names);
        extract_bracket_exports(source, "exports[", names);
        extract_bracket_exports(source, "module.exports[", names);
        extract_define_property_exports(source, "Object.defineProperty(exports,", names);
        extract_define_property_exports(source, "ObjectDefineProperty(exports,", names);
        extract_object_literal_exports(source, names);
    }

    bool read_quoted_string(const std::string &source, size_t *pos, std::string *out)
    {
        skip_whitespace(source, pos);
        if (pos == nullptr || *pos >= source.size() || (source[*pos] != '"' && source[*pos] != '\''))
            return false;
        const char quote = source[(*pos)++];
        const size_t start = *pos;
        while (*pos < source.size())
        {
            if (source[*pos] == '\\')
            {
                *pos += 2;
                continue;
            }
            if (source[*pos] == quote)
            {
                *out = source.substr(start, *pos - start);
                ++(*pos);
                return true;
            }
            ++(*pos);
        }
        return false;
    }

    void extract_literal_requires_in_range(const std::string &source,
                                           size_t start,
                                           size_t end,
                                           std::vector<std::string> *specifiers)
    {
        for (size_t pos = source.find("require", start);
             pos != std::string::npos && pos < end;
             pos = source.find("require", pos + 7))
        {
            if ((pos > 0 && is_identifier_part(source[pos - 1])) ||
                !is_word_boundary(source, pos + 7))
                continue;
            size_t cursor = pos + 7;
            skip_whitespace(source, &cursor);
            if (cursor >= end || source[cursor] != '(')
                continue;
            ++cursor;
            std::string specifier;
            if (read_quoted_string(source, &cursor, &specifier))
                specifiers->push_back(std::move(specifier));
        }
    }

    size_t statement_end(const std::string &source, size_t start)
    {
        size_t end = source.find(';', start);
        if (end == std::string::npos)
            end = source.find('\n', start);
        return end == std::string::npos ? source.size() : end;
    }

    std::vector<std::string> common_js_reexport_specifiers_from_source(const std::string &source)
    {
        std::vector<std::string> specifiers;
        size_t pos = 0;
        while ((pos = source.find("module.exports", pos)) != std::string::npos)
        {
            size_t cursor = pos + std::strlen("module.exports");
            skip_whitespace(source, &cursor);
            if (cursor < source.size() && source[cursor] == '=')
            {
                extract_literal_requires_in_range(source, cursor + 1, statement_end(source, cursor + 1), &specifiers);
            }
            pos = cursor;
        }

        pos = 0;
        while ((pos = source.find("Object.assign", pos)) != std::string::npos)
        {
            size_t end = statement_end(source, pos);
            size_t open = source.find('(', pos);
            if (open != std::string::npos && open < end)
            {
                size_t first_arg = open + 1;
                skip_whitespace(source, &first_arg);
                if (source.compare(first_arg, 7, "exports") == 0 ||
                    source.compare(first_arg, 14, "module.exports") == 0)
                {
                    extract_literal_requires_in_range(source, open + 1, end, &specifiers);
                }
            }
            pos = end;
        }

        pos = 0;
        while ((pos = source.find("__exportStar", pos)) != std::string::npos)
        {
            extract_literal_requires_in_range(source, pos, statement_end(source, pos), &specifiers);
            pos += std::strlen("__exportStar");
        }
        return specifiers;
    }

    bool try_resolve_as_file(const std::filesystem::path &candidate, std::filesystem::path *out)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec)
        {
            *out = std::filesystem::absolute(candidate, ec).lexically_normal();
            if (ec)
                *out = candidate.lexically_normal();
            return true;
        }
        static const char *const extensions[] = {".js", ".cjs", ".json"};
        for (const char *ext : extensions)
        {
            std::filesystem::path with_ext = candidate;
            with_ext += ext;
            if (std::filesystem::is_regular_file(with_ext, ec) && !ec)
            {
                *out = std::filesystem::absolute(with_ext, ec).lexically_normal();
                if (ec)
                    *out = with_ext.lexically_normal();
                return true;
            }
        }
        if (std::filesystem::is_directory(candidate, ec) && !ec)
        {
            return try_resolve_as_file(candidate / "index", out);
        }
        return false;
    }

    bool is_runtime_package_target(const std::string &target)
    {
        if (target.empty() || target.find(".d.ts") != std::string::npos)
            return false;
        const std::filesystem::path path(target);
        const std::string ext = path.extension().string();
        return ext.empty() || ext == ".js" || ext == ".cjs" || ext == ".json";
    }

    void add_json_string_after(const std::string &package_json,
                               const std::string &key,
                               std::vector<std::string> *candidates)
    {
        size_t key_pos = package_json.find("\"" + key + "\"");
        if (key_pos == std::string::npos)
            return;
        size_t colon = package_json.find(':', key_pos);
        size_t quote = package_json.find('"', colon == std::string::npos ? key_pos : colon + 1);
        size_t end = quote == std::string::npos ? std::string::npos : package_json.find('"', quote + 1);
        if (quote != std::string::npos && end != std::string::npos)
            candidates->push_back(package_json.substr(quote + 1, end - quote - 1));
    }

    bool try_resolve_package_entry(const std::filesystem::path &package_dir, std::filesystem::path *out)
    {
        const std::string package_json = read_text_file(package_dir / "package.json");
        std::vector<std::string> candidates;
        add_json_string_after(package_json, "require", &candidates);
        add_json_string_after(package_json, "node", &candidates);
        add_json_string_after(package_json, "default", &candidates);
        add_json_string_after(package_json, "main", &candidates);
        add_json_string_after(package_json, ".", &candidates);
        candidates.push_back("index.js");
        candidates.push_back("index.cjs");
        for (const std::string &candidate : candidates)
        {
            if (is_runtime_package_target(candidate) && try_resolve_as_file(package_dir / candidate, out))
                return true;
        }
        return false;
    }

    bool try_resolve_package_subpath(const std::filesystem::path &package_dir,
                                     const std::string &subpath,
                                     std::filesystem::path *out)
    {
        if (subpath.empty())
            return try_resolve_package_entry(package_dir, out);
        const std::string package_json = read_text_file(package_dir / "package.json");
        const std::string key = "\"./" + subpath + "\"";
        size_t key_pos = package_json.find(key);
        if (key_pos != std::string::npos)
        {
            const size_t search_end = std::min(package_json.size(), key_pos + size_t{900});
            for (const char *condition : {"require", "node", "default"})
            {
                size_t condition_pos = package_json.find(std::string("\"") + condition + "\"", key_pos);
                if (condition_pos == std::string::npos || condition_pos > search_end)
                    continue;
                size_t colon = package_json.find(':', condition_pos);
                size_t quote = package_json.find('"', colon == std::string::npos ? condition_pos : colon + 1);
                size_t end = quote == std::string::npos ? std::string::npos : package_json.find('"', quote + 1);
                if (quote != std::string::npos && end != std::string::npos && end <= search_end)
                {
                    const std::string target = package_json.substr(quote + 1, end - quote - 1);
                    if (is_runtime_package_target(target) && try_resolve_as_file(package_dir / target, out))
                        return true;
                }
            }
        }
        return try_resolve_as_file(package_dir / subpath, out);
    }

    bool try_resolve_package_require(const std::filesystem::path &base_dir,
                                     const std::string &specifier,
                                     std::filesystem::path *out)
    {
        std::string package_name;
        std::string subpath;
        if (starts_with(specifier, "@"))
        {
            const size_t slash = specifier.find('/', specifier.find('/') + 1);
            package_name = slash == std::string::npos ? specifier : specifier.substr(0, slash);
            subpath = slash == std::string::npos ? std::string() : specifier.substr(slash + 1);
        }
        else
        {
            const size_t slash = specifier.find('/');
            package_name = slash == std::string::npos ? specifier : specifier.substr(0, slash);
            subpath = slash == std::string::npos ? std::string() : specifier.substr(slash + 1);
        }

        for (std::filesystem::path dir = base_dir; !dir.empty(); dir = dir.parent_path())
        {
            std::filesystem::path package_dir = dir / "node_modules" / package_name;
            std::error_code ec;
            if (std::filesystem::is_directory(package_dir, ec) && !ec &&
                try_resolve_package_subpath(package_dir, subpath, out))
                return true;
            if (dir == dir.root_path())
                break;
        }
        return false;
    }

    bool try_resolve_require(const std::filesystem::path &from_file,
                             const std::string &specifier,
                             std::filesystem::path *out)
    {
        const std::filesystem::path base_dir = from_file.parent_path();
        if (starts_with(specifier, "./") || starts_with(specifier, "../") || starts_with(specifier, "/"))
            return try_resolve_as_file(base_dir / specifier, out);
        return try_resolve_package_require(base_dir, specifier, out);
    }

    bool should_parse_source_file(const std::filesystem::path &path)
    {
        const std::string ext = path.extension().string();
        return ext.empty() || ext == ".js" || ext == ".cjs";
    }

    std::vector<std::string> export_names_for_file_impl(const std::filesystem::path &filename,
                                                        const std::string *source_arg,
                                                        std::set<std::string> *seen,
                                                        size_t depth)
    {
        std::error_code ec;
        std::filesystem::path absolute = std::filesystem::absolute(filename, ec).lexically_normal();
        if (ec)
            absolute = filename.lexically_normal();
        const std::string key = absolute.string();
        if (depth > kMaxReexportDepth || seen->find(key) != seen->end() || !should_parse_source_file(absolute))
            return {};
        seen->insert(key);

        std::string loaded_source;
        const std::string &source = source_arg != nullptr ? *source_arg : (loaded_source = read_text_file(absolute));
        std::vector<std::string> names;
        extract_direct_export_names(source, &names);

        for (const std::string &specifier : common_js_reexport_specifiers_from_source(source))
        {
            std::filesystem::path resolved;
            if (!try_resolve_require(absolute, specifier, &resolved))
                continue;
            std::vector<std::string> reexport_names = export_names_for_file_impl(resolved, nullptr, seen, depth + 1);
            for (std::string &name : reexport_names)
                add_export_name(&names, std::move(name));
        }
        return names;
    }
}

std::vector<std::string> common_js_export_names_for_file(
    const std::filesystem::path &filename,
    const std::string &source)
{
    std::set<std::string> seen;
    return export_names_for_file_impl(filename, &source, &seen, 0);
}
}
