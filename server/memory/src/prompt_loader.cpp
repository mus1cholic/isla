#include "isla/server/memory/prompt_loader.hpp"

#include <cstddef>
#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

inline constexpr std::size_t kMaxPromptBytes = static_cast<const std::size_t>(64U * 1024U);

struct EmbeddedPromptAsset {
    std::string_view runfile_path;
    std::string_view contents;
};

inline constexpr std::string_view kSystemPromptRunfile =
    "server/memory/include/prompts/system_prompt.txt";
inline constexpr std::string_view kMidTermFlushDeciderSystemPromptRunfile =
    "server/memory/include/prompts/mid_term_flush_decider_system_prompt.txt";
inline constexpr std::string_view kFuturePromptTestRunfile =
    "server/memory/include/prompts/future_prompt_test.txt";

#include "server/memory/src/embedded_prompts.inc"

absl::StatusOr<std::string_view> PromptAssetRunfilePath(PromptAsset prompt_asset) {
    switch (prompt_asset) {
    case PromptAsset::kSystemPrompt:
        return kSystemPromptRunfile;
    case PromptAsset::kMidTermFlushDeciderSystemPrompt:
        return kMidTermFlushDeciderSystemPromptRunfile;
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

absl::Status ValidatePromptContents(std::string_view prompt, std::string_view source_name) {
    if (prompt.empty()) {
        return absl::InvalidArgumentError("prompt must not be empty");
    }
    if (prompt.size() > kMaxPromptBytes) {
        return absl::InvalidArgumentError("prompt exceeds maximum length");
    }
    for (const unsigned char ch : prompt) {
        if (ch == '\0') {
            return absl::InvalidArgumentError("prompt must not contain NUL bytes");
        }
        const bool is_allowed_whitespace = (ch == '\n' || ch == '\r' || ch == '\t');
        if (ch < 0x20 && !is_allowed_whitespace) {
            return absl::InvalidArgumentError("prompt contains unsupported control characters");
        }
    }
    VLOG(2) << "PromptLoader validated prompt source='" << SanitizeForLog(source_name)
            << "' bytes=" << prompt.size();
    return absl::OkStatus();
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
    VLOG(1) << "PromptLoader loaded embedded prompt runfile_path='" << SanitizeForLog(*runfile_path)
            << "' bytes=" << prompt->contents.size();
    absl::Status validation_status = ValidatePromptContents(prompt->contents, *runfile_path);
    if (!validation_status.ok()) {
        VLOG(1) << "PromptLoader rejected embedded prompt runfile_path='"
                << SanitizeForLog(*runfile_path) << "' detail='"
                << SanitizeForLog(validation_status.message()) << "'";
        return validation_status;
    }
    return std::string(prompt->contents);
}

absl::StatusOr<std::string> LoadSystemPrompt() {
    return LoadPrompt(PromptAsset::kSystemPrompt);
}

absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt) {
    if (!configured_prompt.empty()) {
        absl::Status validation_status =
            ValidatePromptContents(configured_prompt, "configured system prompt");
        if (!validation_status.ok()) {
            VLOG(1) << "PromptLoader rejected configured system prompt detail='"
                    << SanitizeForLog(validation_status.message()) << "'";
            return validation_status;
        }
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
