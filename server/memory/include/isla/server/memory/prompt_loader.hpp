#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace isla::server::memory {

inline constexpr std::string_view kSystemPromptRunfile =
    "server/memory/include/prompts/system_prompt.txt";

[[nodiscard]] absl::StatusOr<std::string> LoadPrompt(std::string_view runfile_path);
[[nodiscard]] absl::StatusOr<std::string> LoadSystemPrompt();
[[nodiscard]] absl::StatusOr<std::string> ResolveSystemPrompt(std::string_view configured_prompt);

} // namespace isla::server::memory
