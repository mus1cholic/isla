#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace isla::server::memory {

enum class PromptAsset {
    kSystemPrompt,
    kMidTermCompactorSystemPrompt,
    kMidTermFlushDeciderSystemPrompt,
    kFuturePromptTest,
};

// Loads one embedded prompt asset from the binary and validates that the contents are non-empty,
// bounded in size, and free of unsupported control characters.
[[nodiscard]] absl::StatusOr<std::string> LoadPrompt(PromptAsset prompt_asset);

// Convenience wrapper for the default embedded system prompt asset.
[[nodiscard]] absl::StatusOr<std::string> LoadSystemPrompt();

// Returns the configured system prompt when present after validation; otherwise falls back to the
// embedded default system prompt.
[[nodiscard]] absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt);

} // namespace isla::server::memory
