#include "isla/server/memory/prompt_loader.hpp"

#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

struct EmbeddedPromptAsset {
    std::string_view runfile_path;
    std::string_view contents;
};

inline constexpr std::string_view kSystemPromptRunfile =
    "server/memory/include/prompts/system_prompt.txt";
inline constexpr std::string_view kFuturePromptTestRunfile =
    "server/memory/include/prompts/future_prompt_test.txt";

#include "server/memory/src/embedded_prompts.inc"

absl::StatusOr<std::string_view> PromptAssetRunfilePath(PromptAsset prompt_asset) {
    switch (prompt_asset) {
    case PromptAsset::kSystemPrompt:
        return kSystemPromptRunfile;
    case PromptAsset::kFuturePromptTest:
        return kFuturePromptTestRunfile;
    }

    return absl::InvalidArgumentError("unknown prompt asset");
}

const EmbeddedPromptAsset* FindEmbeddedPrompt(std::string_view runfile_path) {
    for (const EmbeddedPromptAsset& prompt : kEmbeddedPromptAssets) {
        if (prompt.runfile_path == runfile_path) {
            return &prompt;
        }
    }
    return nullptr;
}

} // namespace

absl::StatusOr<std::string> LoadPrompt(PromptAsset prompt_asset) {
    const absl::StatusOr<std::string_view> runfile_path = PromptAssetRunfilePath(prompt_asset);
    if (!runfile_path.ok()) {
        return runfile_path.status();
    }

    const EmbeddedPromptAsset* prompt = FindEmbeddedPrompt(*runfile_path);
    if (prompt == nullptr) {
        VLOG(2) << "PromptLoader missing embedded prompt runfile_path='"
                << SanitizeForLog(*runfile_path) << "'";
        return absl::NotFoundError("prompt was not embedded in the binary");
    }
    VLOG(1) << "PromptLoader loaded embedded prompt runfile_path='"
            << SanitizeForLog(*runfile_path)
            << "' bytes=" << prompt->contents.size();
    return std::string(prompt->contents);
}

absl::StatusOr<std::string> LoadSystemPrompt() {
    return LoadPrompt(PromptAsset::kSystemPrompt);
}

absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt) {
    if (!configured_prompt.empty()) {
        return std::string(configured_prompt);
    }

    absl::StatusOr<std::string> loaded_prompt = LoadSystemPrompt();
    if (!loaded_prompt.ok()) {
        VLOG(1) << "PromptLoader failed to resolve default system prompt detail='"
                << SanitizeForLog(loaded_prompt.status().message()) << "'";
        return loaded_prompt.status();
    }
    return loaded_prompt;
}

} // namespace isla::server::memory
