#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace isla::server::memory {

inline constexpr std::string_view kSystemPromptRunfile =
    "server/memory/include/prompts/system_prompt.txt";

[[nodiscard]] absl::StatusOr<std::string> LoadPrompt(std::string_view runfile_path);
[[nodiscard]] absl::StatusOr<std::string> LoadSystemPrompt();
[[nodiscard]] const std::string& DefaultSystemPrompt();

} // namespace isla::server::memory
