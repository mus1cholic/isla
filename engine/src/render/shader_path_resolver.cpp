#include "engine/src/render/include/shader_path_resolver.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_split.h"

namespace isla::client {

namespace {

std::string find_runfiles_manifest_match(std::string_view manifest_path,
                                         std::string_view relative_path) {
    if (manifest_path.empty() || relative_path.empty()) {
        return "";
    }

    std::ifstream manifest_stream{ std::string(manifest_path) };
    if (!manifest_stream.is_open()) {
        VLOG(1) << "ShaderPathResolver: runfiles manifest not readable at '" << manifest_path
                << "'";
        return "";
    }

    std::string line;
    while (std::getline(manifest_stream, line)) {
        const std::vector<std::string_view> parts = absl::StrSplit(line, absl::MaxSplits(' ', 1));
        if (parts.size() != 2U) {
            continue;
        }
        if (parts.at(0) == relative_path) {
            VLOG(1) << "ShaderPathResolver: manifest '" << manifest_path << "' resolved '"
                    << relative_path << "' to '" << parts.at(1) << "'";
            return std::string(parts.at(1));
        }
    }

    return "";
}

void add_manifest_candidate(std::vector<std::string>& candidates, std::string_view manifest_path,
                            std::string_view runfiles_relative_path) {
    const std::string resolved =
        find_runfiles_manifest_match(manifest_path, runfiles_relative_path);
    if (!resolved.empty()) {
        candidates.push_back(std::filesystem::path(resolved).lexically_normal().string());
    }
}

void add_shader_manifest_candidates(std::vector<std::string>& candidates,
                                    std::string_view manifest_path, std::string_view shader_name) {
    add_manifest_candidate(candidates, manifest_path,
                           std::string("_main/engine/src/render/shaders/dx11/") +
                               std::string(shader_name));
    add_manifest_candidate(candidates, manifest_path,
                           std::string("_main/client/src/shaders/dx11/") +
                               std::string(shader_name));
}

void add_discovered_manifest_candidates(std::vector<std::string>& candidates,
                                        std::string_view executable_base_path,
                                        std::string_view shader_name) {
    if (executable_base_path.empty()) {
        return;
    }

    const std::filesystem::path executable_dir(executable_base_path);
    std::error_code dir_error;
    const std::filesystem::directory_iterator it(executable_dir, dir_error);
    if (dir_error) {
        return;
    }

    for (const std::filesystem::directory_entry& entry : it) {
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) {
            continue;
        }

        const std::filesystem::path file_name = entry.path().filename();
        if (file_name.extension() != ".runfiles_manifest") {
            continue;
        }

        add_shader_manifest_candidates(candidates, entry.path().string(), shader_name);
    }
}

void add_current_working_dir_manifest_candidates(std::vector<std::string>& candidates,
                                                 std::string_view shader_name) {
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        return;
    }

    add_shader_manifest_candidates(candidates, (cwd / "MANIFEST").string(), shader_name);
}

} // namespace

std::vector<std::string> build_shader_path_candidates(const ShaderPathLookup& lookup) {
    std::vector<std::string> candidates;
    const std::string shader_name(lookup.shader_file_name);
    VLOG(1) << "ShaderPathResolver: building candidates for '" << shader_name
            << "', executable_base_path='" << lookup.executable_base_path << "', runfiles_dir='"
            << lookup.runfiles_dir << "', test_srcdir='" << lookup.test_srcdir
            << "', runfiles_manifest_file='" << lookup.runfiles_manifest_file << "'";

    candidates.push_back(std::string("engine/src/render/shaders/dx11/") + shader_name);
    candidates.push_back(std::string("bazel-bin/engine/src/render/shaders/dx11/") + shader_name);
    // Keep legacy paths for compatibility while resolver callers migrate.
    candidates.push_back(std::string("client/src/shaders/dx11/") + shader_name);
    candidates.push_back(std::string("bazel-bin/client/src/shaders/dx11/") + shader_name);

    if (!lookup.runfiles_dir.empty()) {
        const std::filesystem::path runfiles_root(lookup.runfiles_dir);
        candidates.push_back((runfiles_root / "_main" / "engine" / "src" / "render" / "shaders" /
                              "dx11" / shader_name)
                                 .lexically_normal()
                                 .string());
        candidates.push_back(
            (runfiles_root / "_main" / "client" / "src" / "shaders" / "dx11" / shader_name)
                .lexically_normal()
                .string());
    }

    if (!lookup.test_srcdir.empty()) {
        const std::filesystem::path test_src_root(lookup.test_srcdir);
        candidates.push_back((test_src_root / "_main" / "engine" / "src" / "render" / "shaders" /
                              "dx11" / shader_name)
                                 .lexically_normal()
                                 .string());
        candidates.push_back(
            (test_src_root / "_main" / "client" / "src" / "shaders" / "dx11" / shader_name)
                .lexically_normal()
                .string());
    }

    add_shader_manifest_candidates(candidates, lookup.runfiles_manifest_file, shader_name);
    if (lookup.runfiles_manifest_file.empty()) {
        add_current_working_dir_manifest_candidates(candidates, shader_name);
        add_discovered_manifest_candidates(candidates, lookup.executable_base_path, shader_name);
    }

    if (!lookup.executable_base_path.empty()) {
        const std::filesystem::path exe_dir(lookup.executable_base_path);
        candidates.push_back(
            (exe_dir / "shaders" / "dx11" / shader_name).lexically_normal().string());
        candidates.push_back((exe_dir / ".." / ".." / ".." / "engine" / "src" / "render" /
                              "shaders" / "dx11" / shader_name)
                                 .lexically_normal()
                                 .string());
        candidates.push_back((exe_dir / ".." / ".." / ".." / ".." / ".." / "bazel-bin" / "client" /
                              "src" / "shaders" / "dx11" / shader_name)
                                 .lexically_normal()
                                 .string());
        candidates.push_back(
            (exe_dir / ".." / ".." / ".." / "client" / "src" / "shaders" / "dx11" / shader_name)
                .lexically_normal()
                .string());
        candidates.push_back((exe_dir / ".." / ".." / ".." / ".." / ".." / "bazel-bin" / "engine" /
                              "src" / "render" / "shaders" / "dx11" / shader_name)
                                 .lexically_normal()
                                 .string());
    }

    for (const std::string& candidate : candidates) {
        VLOG(1) << "ShaderPathResolver: candidate '" << candidate << "'";
    }

    return candidates;
}

std::string find_existing_shader_path(const std::vector<std::string>& candidates) {
    for (const std::string& candidate : candidates) {
        const std::filesystem::path candidate_path(candidate);
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate_path, error);
        if (error || !exists) {
            continue;
        }
        if (std::filesystem::is_regular_file(candidate_path, error) && !error) {
            LOG(INFO) << "ShaderPathResolver: selected shader path '" << candidate_path.string()
                      << "'";
            return candidate_path.lexically_normal().string();
        }
    }
    LOG(WARNING) << "ShaderPathResolver: no shader path resolved from " << candidates.size()
                 << " candidates";
    return "";
}

std::string find_existing_shader_path(const ShaderPathLookup& lookup) {
    const std::vector<std::string> candidates = build_shader_path_candidates(lookup);
    return find_existing_shader_path(candidates);
}

} // namespace isla::client
