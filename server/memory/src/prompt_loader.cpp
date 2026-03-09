#include "isla/server/memory/prompt_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

std::string sanitize_for_log(std::string_view value) {
    return isla::server::ai_gateway::SanitizeForLog(value);
}

std::vector<std::string> workspace_candidates() {
    std::vector<std::string> candidates;

    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    if (test_workspace != nullptr && test_workspace[0] != '\0') {
        candidates.emplace_back(test_workspace);
    }

    candidates.emplace_back("_main");
    candidates.emplace_back("isla");
    return candidates;
}

absl::StatusOr<std::filesystem::path> ResolveFromManifest(std::string_view manifest_path,
                                                          std::string_view runfile_path) {
    std::ifstream manifest_stream{ std::string(manifest_path) };
    if (!manifest_stream.is_open()) {
        return absl::NotFoundError("runfiles manifest is not readable");
    }

    for (const std::string& workspace : workspace_candidates()) {
        const std::string logical_path = workspace + "/" + std::string(runfile_path);
        std::string line;
        while (std::getline(manifest_stream, line)) {
            if (line.empty() || line.front() == ' ') {
                continue;
            }

            std::istringstream line_stream(line);
            std::string logical;
            line_stream >> logical;
            if (logical != logical_path) {
                continue;
            }

            std::string physical;
            std::getline(line_stream, physical);
            if (!physical.empty() && physical.front() == ' ') {
                physical.erase(0, 1);
            }
            if (!physical.empty()) {
                return std::filesystem::path(physical);
            }
        }
        manifest_stream.clear();
        manifest_stream.seekg(0);
    }

    return absl::NotFoundError("prompt was not found in runfiles manifest");
}

absl::StatusOr<std::filesystem::path> ResolvePromptPath(std::string_view runfile_path) {
    if (const char* runfiles_manifest = std::getenv("RUNFILES_MANIFEST_FILE");
        runfiles_manifest != nullptr && runfiles_manifest[0] != '\0') {
        absl::StatusOr<std::filesystem::path> resolved =
            ResolveFromManifest(runfiles_manifest, runfile_path);
        if (resolved.ok()) {
            VLOG(1) << "PromptLoader resolved prompt via runfiles manifest runfile_path='"
                    << sanitize_for_log(runfile_path) << "' resolved_path='"
                    << sanitize_for_log(resolved->string()) << "'";
            return resolved;
        }
        VLOG(2) << "PromptLoader failed manifest resolution runfile_path='"
                << sanitize_for_log(runfile_path) << "' detail='"
                << sanitize_for_log(resolved.status().message()) << "'";
    }

    const std::vector<std::string> workspaces = workspace_candidates();
    const std::filesystem::path relative_path(runfile_path);
    std::vector<std::filesystem::path> candidates;

    if (const char* test_srcdir = std::getenv("TEST_SRCDIR");
        test_srcdir != nullptr && test_srcdir[0] != '\0') {
        for (const std::string& workspace : workspaces) {
            candidates.push_back(std::filesystem::path(test_srcdir) / workspace / relative_path);
        }
    }

    if (const char* runfiles_dir = std::getenv("RUNFILES_DIR");
        runfiles_dir != nullptr && runfiles_dir[0] != '\0') {
        for (const std::string& workspace : workspaces) {
            candidates.push_back(std::filesystem::path(runfiles_dir) / workspace / relative_path);
        }
    }

    candidates.emplace_back(relative_path);
    candidates.emplace_back(std::filesystem::current_path() / relative_path);

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code exists_error;
        if (std::filesystem::exists(candidate, exists_error) && !exists_error) {
            VLOG(1) << "PromptLoader resolved prompt via filesystem probe runfile_path='"
                    << sanitize_for_log(runfile_path) << "' resolved_path='"
                    << sanitize_for_log(candidate.string()) << "'";
            return candidate;
        }
        if (exists_error) {
            VLOG(2) << "PromptLoader failed filesystem probe runfile_path='"
                    << sanitize_for_log(runfile_path) << "' candidate='"
                    << sanitize_for_log(candidate.string()) << "' detail='"
                    << sanitize_for_log(exists_error.message()) << "'";
        }
    }

    return absl::NotFoundError("unable to resolve bundled prompt path");
}

absl::StatusOr<std::string> ReadPromptFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return absl::NotFoundError("failed to open bundled prompt");
    }

    std::ostringstream content;
    content << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        return absl::DataLossError("failed while reading bundled prompt");
    }
    return content.str();
}

} // namespace

absl::StatusOr<std::string> LoadPrompt(std::string_view runfile_path) {
    absl::StatusOr<std::filesystem::path> prompt_path = ResolvePromptPath(runfile_path);
    if (!prompt_path.ok()) {
        return prompt_path.status();
    }
    absl::StatusOr<std::string> prompt = ReadPromptFile(*prompt_path);
    if (prompt.ok()) {
        VLOG(1) << "PromptLoader loaded prompt runfile_path='"
                << sanitize_for_log(runfile_path) << "' resolved_path='"
                << sanitize_for_log(prompt_path->string())
                << "' bytes=" << prompt->size();
    }
    return prompt;
}

absl::StatusOr<std::string> LoadSystemPrompt() {
    return LoadPrompt(kSystemPromptRunfile);
}

const std::string& DefaultSystemPrompt() {
    static const std::string prompt = [] {
        absl::StatusOr<std::string> loaded_prompt = LoadSystemPrompt();
        if (!loaded_prompt.ok()) {
            LOG(FATAL) << "Failed to load bundled memory system prompt detail='"
                       << sanitize_for_log(loaded_prompt.status().message()) << "'";
        }
        return std::move(*loaded_prompt);
    }();
    return prompt;
}

} // namespace isla::server::memory
