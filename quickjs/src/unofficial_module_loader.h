#ifndef NAPI_QUICKJS_UNOFFICIAL_MODULE_LOADER_H_
#define NAPI_QUICKJS_UNOFFICIAL_MODULE_LOADER_H_

#include <filesystem>
#include <string>

namespace edge_quickjs::module_loader {

std::filesystem::path ResolveSymlinkComponents(const std::filesystem::path& path);
std::filesystem::path NormalizeResolvedPath(const std::filesystem::path& path);
std::string ReadTextFile(const std::filesystem::path& path);
bool PackageTypeIsModule(const std::filesystem::path& package_json_path);

bool IsRegularFileFollowingSymlinks(const std::filesystem::path& candidate,
                                    std::filesystem::path* out);
bool IsDirectoryFollowingSymlinks(const std::filesystem::path& candidate,
                                  std::filesystem::path* out);

bool ResolveCommonJSPath(const std::string& specifier,
                         const std::string& base_dir,
                         std::filesystem::path* out);

bool ResolveESMPath(const std::string& base,
                    const std::string& specifier,
                    std::string* out);

}  // namespace edge_quickjs::module_loader

#endif  // NAPI_QUICKJS_UNOFFICIAL_MODULE_LOADER_H_
