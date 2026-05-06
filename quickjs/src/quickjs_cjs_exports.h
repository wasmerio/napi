#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quickjs_napi
{
    std::vector<std::string> common_js_export_names_for_file(
        const std::filesystem::path &filename,
        const std::string &source);
}
