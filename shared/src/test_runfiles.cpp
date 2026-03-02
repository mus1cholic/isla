#include "shared/src/test_runfiles.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if __has_include("rules_cc/cc/runfiles/runfiles.h")
#include "rules_cc/cc/runfiles/runfiles.h"
#define ISLA_HAS_RULES_CC_RUNFILES 1
#else
#define ISLA_HAS_RULES_CC_RUNFILES 0
#endif

namespace isla::shared::test {

namespace {

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

} // namespace

std::string runfile_path(std::string_view relative_workspace_path) {
#if ISLA_HAS_RULES_CC_RUNFILES
    std::string error;
    std::unique_ptr<rules_cc::cc::runfiles::Runfiles> runfiles(
        rules_cc::cc::runfiles::Runfiles::CreateForTest(&error));
    if (runfiles != nullptr) {
        for (const std::string& workspace : workspace_candidates()) {
            std::string logical_path = workspace;
            logical_path += "/";
            logical_path += relative_workspace_path;
            const std::string resolved = runfiles->Rlocation(logical_path);
            if (!resolved.empty()) {
                return resolved;
            }
        }
    }
#endif

    const char* runfiles_manifest_file = std::getenv("RUNFILES_MANIFEST_FILE");
    if (runfiles_manifest_file != nullptr && runfiles_manifest_file[0] != '\0') {
        std::ifstream manifest_stream(runfiles_manifest_file);
        if (manifest_stream.is_open()) {
            for (const std::string& workspace : workspace_candidates()) {
                const std::string logical_path =
                    workspace + "/" + std::string(relative_workspace_path);
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
                        return physical;
                    }
                }
                manifest_stream.clear();
                manifest_stream.seekg(0);
            }
        }
    }

    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir != nullptr && test_srcdir[0] != '\0') {
        for (const std::string& workspace : workspace_candidates()) {
            std::string path = test_srcdir;
            path += "/";
            path += workspace;
            path += "/";
            path += relative_workspace_path;
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    return std::string(relative_workspace_path);
}

} // namespace isla::shared::test


