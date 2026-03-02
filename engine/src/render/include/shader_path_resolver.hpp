#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace isla::client {

struct ShaderPathLookup {
    std::string_view shader_file_name;
    std::string_view executable_base_path;
    std::string_view runfiles_dir;
    std::string_view test_srcdir;
    std::string_view runfiles_manifest_file;
};

std::vector<std::string> build_shader_path_candidates(const ShaderPathLookup& lookup);
std::string find_existing_shader_path(const std::vector<std::string>& candidates);
std::string find_existing_shader_path(const ShaderPathLookup& lookup);

} // namespace isla::client
