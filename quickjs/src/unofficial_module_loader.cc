#include "unofficial_module_loader.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace edge_quickjs::module_loader {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxSymlinkExpansions = 64;

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

int HexDigitValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

std::string PercentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const int hi = HexDigitValue(input[i + 1]);
      const int lo = HexDigitValue(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

fs::path StripFileUrl(std::string_view value) {
  constexpr std::string_view kScheme = "file://";
  if (!StartsWith(value, kScheme)) return fs::path(std::string(value));

  std::string rest(value.substr(kScheme.size()));
  if (StartsWith(rest, "localhost/")) {
    rest.erase(0, std::string("localhost").size());
  } else if (!rest.empty() && rest[0] != '/') {
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return {};
    rest.erase(0, slash);
  }
  return fs::path(PercentDecode(rest));
}

std::string PathToFileUrl(const fs::path& path) {
  std::string out = "file://";
  const std::string input = path.string();
  for (char ch : input) {
    if (ch == ' ') {
      out += "%20";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

struct JsonValue {
  enum class Type { kMissing, kNull, kString, kArray, kObject, kOther };
  Type type = Type::kMissing;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  const JsonValue* Get(std::string_view key) const {
    if (type != Type::kObject) return nullptr;
    for (const auto& entry : object) {
      if (entry.first == key) return &entry.second;
    }
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view source) : source_(source) {}

  bool Parse(JsonValue* out) {
    if (out == nullptr) return false;
    SkipWhitespace();
    if (!ParseValue(out)) return false;
    SkipWhitespace();
    return pos_ == source_.size();
  }

 private:
  void SkipWhitespace() {
    while (pos_ < source_.size()) {
      const char ch = source_[pos_];
      if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
      ++pos_;
    }
  }

  bool Consume(char expected) {
    SkipWhitespace();
    if (pos_ >= source_.size() || source_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ParseString(std::string* out) {
    SkipWhitespace();
    if (pos_ >= source_.size() || source_[pos_] != '"') return false;
    ++pos_;
    std::string value;
    while (pos_ < source_.size()) {
      const char ch = source_[pos_++];
      if (ch == '"') {
        *out = std::move(value);
        return true;
      }
      if (ch != '\\') {
        value.push_back(ch);
        continue;
      }
      if (pos_ >= source_.size()) return false;
      const char escaped = source_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value.push_back(escaped);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u':
          if (pos_ + 4 > source_.size()) return false;
          value.push_back('?');
          pos_ += 4;
          break;
        default:
          return false;
      }
    }
    return false;
  }

  bool ParseArray(JsonValue* out) {
    if (!Consume('[')) return false;
    out->type = JsonValue::Type::kArray;
    SkipWhitespace();
    if (pos_ < source_.size() && source_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue item;
      if (!ParseValue(&item)) return false;
      out->array.push_back(std::move(item));
      SkipWhitespace();
      if (pos_ < source_.size() && source_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseObject(JsonValue* out) {
    if (!Consume('{')) return false;
    out->type = JsonValue::Type::kObject;
    SkipWhitespace();
    if (pos_ < source_.size() && source_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      std::string key;
      if (!ParseString(&key) || !Consume(':')) return false;
      JsonValue value;
      if (!ParseValue(&value)) return false;
      out->object.emplace_back(std::move(key), std::move(value));
      SkipWhitespace();
      if (pos_ < source_.size() && source_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseValue(JsonValue* out) {
    SkipWhitespace();
    if (pos_ >= source_.size()) return false;
    const char ch = source_[pos_];
    if (ch == '"') {
      out->type = JsonValue::Type::kString;
      return ParseString(&out->string);
    }
    if (ch == '{') return ParseObject(out);
    if (ch == '[') return ParseArray(out);
    if (StartsWith(source_.substr(pos_), "null")) {
      pos_ += 4;
      out->type = JsonValue::Type::kNull;
      return true;
    }
    if (StartsWith(source_.substr(pos_), "true")) {
      pos_ += 4;
      out->type = JsonValue::Type::kOther;
      return true;
    }
    if (StartsWith(source_.substr(pos_), "false")) {
      pos_ += 5;
      out->type = JsonValue::Type::kOther;
      return true;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
      ++pos_;
      while (pos_ < source_.size()) {
        const char next = source_[pos_];
        if (!((next >= '0' && next <= '9') || next == '.' || next == 'e' ||
              next == 'E' || next == '+' || next == '-')) {
          break;
        }
        ++pos_;
      }
      out->type = JsonValue::Type::kOther;
      return true;
    }
    return false;
  }

  std::string_view source_;
  size_t pos_ = 0;
};

struct PackageConfig {
  bool exists = false;
  fs::path package_json_path;
  fs::path package_dir;
  std::optional<std::string> name;
  std::optional<std::string> main;
  const JsonValue* imports = nullptr;
  const JsonValue* exports_field = nullptr;
  JsonValue document;
};

bool ReadPackageConfig(const fs::path& package_json_path, PackageConfig* out) {
  if (out == nullptr) return false;
  fs::path real_package_json;
  if (!IsRegularFileFollowingSymlinks(package_json_path, &real_package_json)) {
    return false;
  }

  const std::string source = ReadTextFile(real_package_json);
  if (source.empty()) return false;

  JsonValue document;
  JsonParser parser(source);
  if (!parser.Parse(&document) || document.type != JsonValue::Type::kObject) {
    return false;
  }

  PackageConfig config;
  config.exists = true;
  config.package_json_path = real_package_json;
  config.package_dir = real_package_json.parent_path();
  config.document = std::move(document);

  if (const JsonValue* name = config.document.Get("name");
      name != nullptr && name->type == JsonValue::Type::kString) {
    config.name = name->string;
  }
  if (const JsonValue* main = config.document.Get("main");
      main != nullptr && main->type == JsonValue::Type::kString) {
    config.main = main->string;
  }
  config.imports = config.document.Get("imports");
  config.exports_field = config.document.Get("exports");
  *out = std::move(config);
  return true;
}

bool IsRelativeSpecifier(std::string_view specifier) {
  if (specifier.empty() || specifier[0] != '.') return false;
  return specifier.size() == 1 || specifier[1] == '/' ||
         (specifier[1] == '.' && (specifier.size() == 2 || specifier[2] == '/'));
}

bool IsRelativeOrAbsoluteRequest(std::string_view specifier) {
  return !specifier.empty() && (specifier[0] == '/' || IsRelativeSpecifier(specifier));
}

bool HasTrailingSlash(std::string_view request) {
  if (request.empty()) return false;
  if (request.back() == '/') return true;
  if (request == "." || request == "..") return true;
  return EndsWith(request, "/.") || EndsWith(request, "/..");
}

bool TryResolveAsFileWithExtensions(const fs::path& candidate,
                                    const std::vector<std::string>& extensions,
                                    fs::path* out) {
  if (IsRegularFileFollowingSymlinks(candidate, out)) return true;
  for (const std::string& ext : extensions) {
    fs::path with_ext = candidate;
    with_ext += ext;
    if (IsRegularFileFollowingSymlinks(with_ext, out)) return true;
  }
  return false;
}

bool TryResolveAsDirectory(const fs::path& candidate,
                           const std::vector<std::string>& extensions,
                           fs::path* out) {
  fs::path directory;
  if (!IsDirectoryFollowingSymlinks(candidate, &directory)) return false;

  PackageConfig package_config;
  if (ReadPackageConfig(directory / "package.json", &package_config) &&
      package_config.main.has_value() && !package_config.main->empty()) {
    const fs::path main_candidate = directory / *package_config.main;
    if (TryResolveAsFileWithExtensions(main_candidate, extensions, out) ||
        TryResolveAsDirectory(main_candidate, extensions, out)) {
      return true;
    }
  }

  return TryResolveAsFileWithExtensions(directory / "index", extensions, out);
}

std::optional<std::pair<std::string, std::string>> ParsePackageRequest(
    std::string_view request) {
  if (request.empty() || request[0] == '.' || request[0] == '/' ||
      request[0] == '\\' || request[0] == '%') {
    return std::nullopt;
  }
  size_t name_end = request.find('/');
  if (request[0] == '@') {
    const size_t scope_end = request.find('/');
    if (scope_end == std::string_view::npos) return std::nullopt;
    name_end = request.find('/', scope_end + 1);
  }
  const std::string name = name_end == std::string_view::npos
                               ? std::string(request)
                               : std::string(request.substr(0, name_end));
  if (name.empty() || name.find('\\') != std::string::npos ||
      name.find('%') != std::string::npos) {
    return std::nullopt;
  }
  const std::string subpath = name_end == std::string_view::npos
                                  ? "."
                                  : "." + std::string(request.substr(name_end));
  return std::make_pair(name, subpath);
}

bool IsConditionalExportsMainSugar(const JsonValue& exports_value) {
  if (exports_value.type == JsonValue::Type::kString ||
      exports_value.type == JsonValue::Type::kArray) {
    return true;
  }
  if (exports_value.type != JsonValue::Type::kObject) return false;
  bool first = true;
  bool is_conditional = false;
  for (const auto& entry : exports_value.object) {
    const bool current = entry.first.empty() || entry.first[0] != '.';
    if (first) {
      is_conditional = current;
      first = false;
    } else if (is_conditional != current) {
      return false;
    }
  }
  return is_conditional;
}

int PatternKeyCompare(std::string_view a, std::string_view b) {
  const size_t a_star = a.find('*');
  const size_t b_star = b.find('*');
  const size_t a_base_len = a_star == std::string_view::npos ? a.size() : a_star + 1;
  const size_t b_base_len = b_star == std::string_view::npos ? b.size() : b_star + 1;
  if (a_base_len > b_base_len) return -1;
  if (b_base_len > a_base_len) return 1;
  if (a_star == std::string_view::npos) return 1;
  if (b_star == std::string_view::npos) return -1;
  if (a.size() > b.size()) return -1;
  if (b.size() > a.size()) return 1;
  return 0;
}

std::string ReplaceStars(std::string value, std::string_view subpath) {
  for (size_t star = value.find('*'); star != std::string::npos;
       star = value.find('*', star + subpath.size())) {
    value.replace(star, 1, subpath);
  }
  return value;
}

bool HasInvalidPackageSegment(const fs::path& path) {
  for (const fs::path& part : path.lexically_normal()) {
    const std::string text = part.string();
    if (text == ".." || text == "node_modules") return true;
  }
  return false;
}

bool ResolvePackageTarget(const fs::path& package_dir,
                          const JsonValue& target,
                          std::string_view subpath,
                          std::string_view package_subpath,
                          const std::set<std::string>& conditions,
                          bool pattern,
                          bool internal,
                          const std::vector<std::string>& extensions,
                          fs::path* out);

bool ResolvePackageTargetString(const fs::path& package_dir,
                                std::string_view target,
                                std::string_view subpath,
                                std::string_view package_subpath,
                                const std::set<std::string>& conditions,
                                bool pattern,
                                bool internal,
                                const std::vector<std::string>& extensions,
                                fs::path* out) {
  std::string expanded = ReplaceStars(std::string(target), subpath);
  if (pattern) {
    expanded = ReplaceStars(expanded, subpath);
  }

  if (!StartsWith(expanded, "./")) {
    if (internal && !expanded.empty() && expanded[0] != '/' &&
        !IsRelativeSpecifier(expanded)) {
      return ResolveCommonJSPath(expanded, package_dir.string(), out);
    }
    return false;
  }

  fs::path relative = fs::path(expanded.substr(2));
  if (HasInvalidPackageSegment(relative)) return false;
  if (!subpath.empty() && HasInvalidPackageSegment(fs::path(std::string(subpath)))) {
    return false;
  }

  fs::path resolved = package_dir / relative;
  if (pattern && !subpath.empty()) {
    resolved = fs::path(ReplaceStars(resolved.string(), subpath));
  } else if (!pattern && !subpath.empty()) {
    resolved /= std::string(subpath);
  }

  return TryResolveAsFileWithExtensions(resolved, extensions, out);
}

bool ResolvePackageTarget(const fs::path& package_dir,
                          const JsonValue& target,
                          std::string_view subpath,
                          std::string_view package_subpath,
                          const std::set<std::string>& conditions,
                          bool pattern,
                          bool internal,
                          const std::vector<std::string>& extensions,
                          fs::path* out) {
  switch (target.type) {
    case JsonValue::Type::kString:
      return ResolvePackageTargetString(package_dir,
                                        target.string,
                                        subpath,
                                        package_subpath,
                                        conditions,
                                        pattern,
                                        internal,
                                        extensions,
                                        out);
    case JsonValue::Type::kArray:
      for (const JsonValue& item : target.array) {
        if (ResolvePackageTarget(package_dir,
                                 item,
                                 subpath,
                                 package_subpath,
                                 conditions,
                                 pattern,
                                 internal,
                                 extensions,
                                 out)) {
          return true;
        }
      }
      return false;
    case JsonValue::Type::kObject:
      for (const auto& entry : target.object) {
        if (entry.first == "default" || conditions.count(entry.first) != 0) {
          if (ResolvePackageTarget(package_dir,
                                   entry.second,
                                   subpath,
                                   package_subpath,
                                   conditions,
                                   pattern,
                                   internal,
                                   extensions,
                                   out)) {
            return true;
          }
        }
      }
      return false;
    default:
      return false;
  }
}

bool PackageExportsResolve(const fs::path& package_dir,
                           const JsonValue& exports_field,
                           std::string package_subpath,
                           const std::set<std::string>& conditions,
                           const std::vector<std::string>& extensions,
                           fs::path* out) {
  JsonValue sugar;
  const JsonValue* exports_value = &exports_field;
  if (IsConditionalExportsMainSugar(exports_field)) {
    sugar.type = JsonValue::Type::kObject;
    sugar.object.emplace_back(".", exports_field);
    exports_value = &sugar;
  }
  if (exports_value->type != JsonValue::Type::kObject) return false;

  if (const JsonValue* exact = exports_value->Get(package_subpath);
      exact != nullptr && package_subpath.find('*') == std::string::npos &&
      !EndsWith(package_subpath, "/")) {
    return ResolvePackageTarget(package_dir,
                                *exact,
                                "",
                                package_subpath,
                                conditions,
                                false,
                                false,
                                extensions,
                                out);
  }

  std::string best_match;
  std::string best_subpath;
  const JsonValue* best_target = nullptr;
  for (const auto& entry : exports_value->object) {
    const std::string& key = entry.first;
    const size_t pattern_index = key.find('*');
    if (pattern_index == std::string::npos) continue;
    if (!StartsWith(package_subpath, std::string_view(key).substr(0, pattern_index))) {
      continue;
    }
    const std::string_view trailer(key.c_str() + pattern_index + 1,
                                   key.size() - pattern_index - 1);
    if (package_subpath.size() < key.size() || !EndsWith(package_subpath, trailer) ||
        key.find('*', pattern_index + 1) != std::string::npos) {
      continue;
    }
    if (best_match.empty() || PatternKeyCompare(best_match, key) == 1) {
      best_match = key;
      best_subpath = package_subpath.substr(
          pattern_index, package_subpath.size() - pattern_index - trailer.size());
      best_target = &entry.second;
    }
  }

  if (best_target == nullptr) return false;
  return ResolvePackageTarget(package_dir,
                              *best_target,
                              best_subpath,
                              best_match,
                              conditions,
                              true,
                              false,
                              extensions,
                              out);
}

bool PackageImportsResolve(const std::string& specifier,
                           const fs::path& base_dir,
                           const std::set<std::string>& conditions,
                           const std::vector<std::string>& extensions,
                           fs::path* out) {
  if (specifier == "#" || StartsWith(specifier, "#/") || EndsWith(specifier, "/")) {
    return false;
  }

  for (fs::path dir = base_dir; !dir.empty(); dir = dir.parent_path()) {
    if (dir.filename() == "node_modules") return false;
    PackageConfig package_config;
    if (ReadPackageConfig(dir / "package.json", &package_config) &&
        package_config.imports != nullptr &&
        package_config.imports->type == JsonValue::Type::kObject) {
      if (const JsonValue* exact = package_config.imports->Get(specifier);
          exact != nullptr && specifier.find('*') == std::string::npos) {
        if (ResolvePackageTarget(package_config.package_dir,
                                 *exact,
                                 "",
                                 specifier,
                                 conditions,
                                 false,
                                 true,
                                 extensions,
                                 out)) {
          return true;
        }
      }

      std::string best_match;
      std::string best_subpath;
      const JsonValue* best_target = nullptr;
      for (const auto& entry : package_config.imports->object) {
        const std::string& key = entry.first;
        const size_t pattern_index = key.find('*');
        if (pattern_index == std::string::npos ||
            !StartsWith(specifier, std::string_view(key).substr(0, pattern_index))) {
          continue;
        }
        const std::string_view trailer(key.c_str() + pattern_index + 1,
                                       key.size() - pattern_index - 1);
        if (specifier.size() < key.size() || !EndsWith(specifier, trailer) ||
            key.find('*', pattern_index + 1) != std::string::npos) {
          continue;
        }
        if (best_match.empty() || PatternKeyCompare(best_match, key) == 1) {
          best_match = key;
          best_subpath = specifier.substr(
              pattern_index, specifier.size() - pattern_index - trailer.size());
          best_target = &entry.second;
        }
      }
      if (best_target != nullptr &&
          ResolvePackageTarget(package_config.package_dir,
                               *best_target,
                               best_subpath,
                               best_match,
                               conditions,
                               true,
                               true,
                               extensions,
                               out)) {
        return true;
      }
    }

    if (dir == dir.root_path()) break;
  }
  return false;
}

bool TrySelf(const fs::path& base_dir,
             const std::string& request,
             const std::set<std::string>& conditions,
             const std::vector<std::string>& extensions,
             fs::path* out) {
  for (fs::path dir = base_dir; !dir.empty(); dir = dir.parent_path()) {
    if (dir.filename() == "node_modules") return false;
    PackageConfig package_config;
    if (ReadPackageConfig(dir / "package.json", &package_config) &&
        package_config.exports_field != nullptr && package_config.name.has_value()) {
      std::string package_subpath;
      if (request == *package_config.name) {
        package_subpath = ".";
      } else if (StartsWith(request, *package_config.name + "/")) {
        package_subpath = "." + request.substr(package_config.name->size());
      } else {
        return false;
      }
      return PackageExportsResolve(package_config.package_dir,
                                   *package_config.exports_field,
                                   package_subpath,
                                   conditions,
                                   extensions,
                                   out);
    }
    if (dir == dir.root_path()) break;
  }
  return false;
}

std::vector<fs::path> NodeModulePaths(const fs::path& from_dir) {
  fs::path from = fs::absolute(from_dir).lexically_normal();
  if (from == "/") return {fs::path("/node_modules")};

  std::vector<fs::path> paths;
  const std::string from_str = from.string();
  const std::string node_modules = "node_modules";
  size_t last = from_str.size();
  for (size_t i = from_str.size(); i > 0; --i) {
    const size_t idx = i - 1;
    if (from_str[idx] != '/') continue;
    const std::string segment = from_str.substr(idx + 1, last - idx - 1);
    if (segment != node_modules) {
      paths.push_back(fs::path(from_str.substr(0, last)) / "node_modules");
    }
    last = idx;
  }
  paths.push_back(fs::path("/node_modules"));
  return paths;
}

bool ResolveExportsFromNodeModulesDir(const fs::path& nm_dir,
                                      const std::string& request,
                                      const std::set<std::string>& conditions,
                                      const std::vector<std::string>& extensions,
                                      fs::path* out) {
  const auto parsed = ParsePackageRequest(request);
  if (!parsed.has_value()) return false;

  const fs::path package_dir = nm_dir / parsed->first;
  PackageConfig package_config;
  if (!ReadPackageConfig(package_dir / "package.json", &package_config) ||
      package_config.exports_field == nullptr) {
    return false;
  }
  return PackageExportsResolve(package_config.package_dir,
                               *package_config.exports_field,
                               parsed->second,
                               conditions,
                               extensions,
                               out);
}

bool FindPath(const std::string& request,
              const std::vector<fs::path>& paths,
              const std::set<std::string>& conditions,
              const std::vector<std::string>& extensions,
              fs::path* out) {
  const bool absolute_request = !request.empty() && request[0] == '/';
  const bool trailing_slash = HasTrailingSlash(request);

  for (const fs::path& cur_path : paths) {
    fs::path cur_dir;
    if (!cur_path.empty() && !IsDirectoryFollowingSymlinks(cur_path, &cur_dir)) {
      continue;
    }

    if (!absolute_request &&
        ResolveExportsFromNodeModulesDir(cur_path, request, conditions, extensions, out)) {
      return true;
    }

    const fs::path base_path = absolute_request ? fs::path(request) : cur_path / request;
    if (!trailing_slash && TryResolveAsFileWithExtensions(base_path, extensions, out)) {
      return true;
    }
    if (TryResolveAsDirectory(base_path, extensions, out)) {
      return true;
    }
  }

  return false;
}

bool ResolvePackageSubpathForESM(const fs::path& package_dir,
                                 const std::string& subpath,
                                 fs::path* out) {
  static const std::set<std::string> conditions = {"import", "module", "node"};
  static const std::vector<std::string> extensions = {".js", ".mjs", ".json"};

  PackageConfig package_config;
  if (ReadPackageConfig(package_dir / "package.json", &package_config) &&
      package_config.exports_field != nullptr &&
      PackageExportsResolve(package_config.package_dir,
                            *package_config.exports_field,
                            subpath.empty() ? "." : "./" + subpath,
                            conditions,
                            extensions,
                            out)) {
    return true;
  }

  if (subpath.empty()) {
    if (package_config.main.has_value() &&
        TryResolveAsFileWithExtensions(package_config.package_dir / *package_config.main,
                                       extensions,
                                       out)) {
      return true;
    }
    return TryResolveAsFileWithExtensions(package_dir / "index", extensions, out);
  }

  const fs::path subpath_candidate = package_dir / subpath;
  if (TryResolveAsFileWithExtensions(subpath_candidate, extensions, out) ||
      TryResolveAsDirectory(subpath_candidate, extensions, out)) {
    return true;
  }
  return false;
}

}  // namespace

fs::path ResolveSymlinkComponents(const fs::path& path) {
  fs::path current;
  size_t expansions = 0;
  std::unordered_set<std::string> seen;

  for (const fs::path& part : path.lexically_normal()) {
    if (part == "." || part.empty()) continue;
    if (part == "..") {
      current /= part;
      continue;
    }
    if (part == path.root_name() || part == path.root_directory()) {
      current /= part;
      continue;
    }

    fs::path candidate = current.empty() ? part : current / part;
    std::error_code ec;
    if (fs::is_symlink(candidate, ec) && !ec) {
      const std::string key = candidate.lexically_normal().string();
      if (++expansions > kMaxSymlinkExpansions || !seen.insert(key).second) {
        current = candidate;
        continue;
      }
      fs::path target = fs::read_symlink(candidate, ec);
      if (!ec) {
        current = target.is_absolute() ? target : candidate.parent_path() / target;
        current = current.lexically_normal();
        continue;
      }
    }
    current = candidate;
  }
  return current.lexically_normal();
}

fs::path NormalizeResolvedPath(const fs::path& path) {
  std::error_code ec;
  fs::path absolute = path;
  if (!absolute.is_absolute()) {
    absolute = fs::absolute(path, ec);
    if (ec) {
      absolute = path;
      ec.clear();
    }
  }
  fs::path canonical = fs::weakly_canonical(absolute, ec);
  if (!ec) return canonical.lexically_normal();
  fs::path resolved = ResolveSymlinkComponents(absolute);
  if (resolved != absolute.lexically_normal()) return resolved.lexically_normal();
  return absolute.lexically_normal();
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    const fs::path resolved = ResolveSymlinkComponents(path);
    if (resolved != path.lexically_normal()) {
      in.clear();
      in.open(resolved);
    }
  }
  if (!in.is_open()) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool PackageTypeIsModule(const fs::path& package_json_path) {
  PackageConfig package_config;
  if (!ReadPackageConfig(package_json_path, &package_config)) return false;
  const JsonValue* type = package_config.document.Get("type");
  return type != nullptr && type->type == JsonValue::Type::kString &&
         type->string == "module";
}

bool IsRegularFileFollowingSymlinks(const fs::path& candidate, fs::path* out) {
  std::error_code ec;
  if (fs::is_regular_file(candidate, ec) && !ec) {
    if (out != nullptr) *out = NormalizeResolvedPath(candidate);
    return true;
  }
  const fs::path resolved = ResolveSymlinkComponents(candidate);
  if (resolved != candidate.lexically_normal()) {
    ec.clear();
    if (fs::is_regular_file(resolved, ec) && !ec) {
      if (out != nullptr) *out = NormalizeResolvedPath(resolved);
      return true;
    }
  }
  return false;
}

bool IsDirectoryFollowingSymlinks(const fs::path& candidate, fs::path* out) {
  std::error_code ec;
  if (fs::is_directory(candidate, ec) && !ec) {
    if (out != nullptr) *out = NormalizeResolvedPath(candidate);
    return true;
  }
  const fs::path resolved = ResolveSymlinkComponents(candidate);
  if (resolved != candidate.lexically_normal()) {
    ec.clear();
    if (fs::is_directory(resolved, ec) && !ec) {
      if (out != nullptr) *out = NormalizeResolvedPath(resolved);
      return true;
    }
  }
  return false;
}

bool ResolveCommonJSPath(const std::string& specifier,
                         const std::string& base_dir,
                         fs::path* out) {
  if (out == nullptr || specifier.empty()) return false;

  static const std::set<std::string> conditions = {"require", "node", "node-addons"};
  static const std::vector<std::string> extensions = {".js", ".json", ".node"};

  fs::path base = base_dir.empty() ? fs::current_path() : fs::path(base_dir);
  base = NormalizeResolvedPath(base);

  if (specifier[0] == '#') {
    if (PackageImportsResolve(specifier, base, conditions, extensions, out)) return true;
  }

  if (!IsRelativeOrAbsoluteRequest(specifier)) {
    if (TrySelf(base, specifier, conditions, extensions, out)) return true;
    return FindPath(specifier, NodeModulePaths(base), conditions, extensions, out);
  }

  const fs::path candidate = specifier[0] == '/' ? fs::path(specifier) : base / specifier;
  return FindPath(candidate.string(), {fs::path()}, conditions, extensions, out);
}

bool ResolveESMPath(const std::string& base,
                    const std::string& specifier,
                    std::string* out) {
  if (out == nullptr || specifier.empty()) return false;

  fs::path base_path = StripFileUrl(base);
  fs::path base_dir = fs::current_path();
  std::error_code ec;
  if (!base_path.empty() && base_path.is_absolute()) {
    base_dir = fs::is_directory(base_path, ec) ? base_path : base_path.parent_path();
  }
  base_dir = NormalizeResolvedPath(base_dir);

  fs::path resolved;
  if (specifier[0] == '#') {
    static const std::set<std::string> conditions = {"import", "module", "node"};
    static const std::vector<std::string> extensions = {".js", ".mjs", ".json"};
    if (PackageImportsResolve(specifier, base_dir, conditions, extensions, &resolved)) {
      *out = resolved.string();
      return true;
    }
    return false;
  }

  if (StartsWith(specifier, "file://")) {
    static const std::vector<std::string> extensions = {".js", ".mjs", ".json"};
    if (TryResolveAsFileWithExtensions(StripFileUrl(specifier), extensions, &resolved)) {
      *out = resolved.string();
      return true;
    }
    return false;
  }

  if (IsRelativeOrAbsoluteRequest(specifier)) {
    static const std::vector<std::string> extensions = {".js", ".mjs", ".json"};
    const fs::path candidate = specifier[0] == '/' ? fs::path(specifier) : base_dir / specifier;
    if (TryResolveAsFileWithExtensions(candidate, extensions, &resolved) ||
        TryResolveAsDirectory(candidate, extensions, &resolved)) {
      *out = resolved.string();
      return true;
    }
    return false;
  }

  std::string package_name = specifier;
  std::string subpath;
  size_t slash = specifier[0] == '@' ? specifier.find('/', specifier.find('/') + 1)
                                    : specifier.find('/');
  if (slash != std::string::npos) {
    package_name = specifier.substr(0, slash);
    subpath = specifier.substr(slash + 1);
  }
  if (specifier == "es-module-lexer") subpath = "js";

  for (const fs::path& nm_dir : NodeModulePaths(base_dir)) {
    if (ResolvePackageSubpathForESM(nm_dir / package_name, subpath, &resolved)) {
      *out = resolved.string();
      return true;
    }
  }
  return false;
}

}  // namespace edge_quickjs::module_loader
