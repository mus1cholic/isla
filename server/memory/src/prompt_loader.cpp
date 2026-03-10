#include "isla/server/memory/prompt_loader.hpp"

#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

struct EmbeddedPromptAsset {
    std::string_view runfile_path;
    std::string_view contents;
};

#include "server/memory/src/embedded_prompts.inc"

std::string sanitize_for_log(std::string_view value) {
    return isla::server::ai_gateway::SanitizeForLog(value);
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

absl::StatusOr<std::string> LoadPrompt(std::string_view runfile_path) {
    const EmbeddedPromptAsset* prompt = FindEmbeddedPrompt(runfile_path);
    if (prompt == nullptr) {
        VLOG(2) << "PromptLoader missing embedded prompt runfile_path='"
                << sanitize_for_log(runfile_path) << "'";
        return absl::NotFoundError("prompt was not embedded in the binary");
    }
    VLOG(1) << "PromptLoader loaded embedded prompt runfile_path='"
            << sanitize_for_log(runfile_path) << "' bytes=" << prompt->contents.size();
    return std::string(prompt->contents);
}

absl::StatusOr<std::string> LoadSystemPrompt() {
    return LoadPrompt(kSystemPromptRunfile);
}

absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt) {
    if (!configured_prompt.empty()) {
        return std::string(configured_prompt);
    }

    absl::StatusOr<std::string> loaded_prompt = LoadSystemPrompt();
    if (!loaded_prompt.ok()) {
        VLOG(1) << "PromptLoader failed to resolve default system prompt detail='"
                << sanitize_for_log(loaded_prompt.status().message()) << "'";
        return loaded_prompt.status();
    }
    return loaded_prompt;
}

} // namespace isla::server::memory
