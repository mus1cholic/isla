#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace isla::server::memory {

enum class PromptAsset {
    kSystemPrompt,
    kFuturePromptTest,
};

[[nodiscard]] absl::StatusOr<std::string> LoadPrompt(PromptAsset prompt_asset);
[[nodiscard]] absl::StatusOr<std::string> LoadSystemPrompt();
[[nodiscard]] absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt);

} // namespace isla::server::memory
