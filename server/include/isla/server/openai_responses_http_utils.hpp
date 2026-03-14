#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"

namespace isla::server::ai_gateway {

absl::Status ValidateHttpFieldValue(std::string_view field_name, std::string_view value);
absl::Status ValidateHttpHostValue(std::string_view value);
absl::Status ValidateHttpTargetValue(std::string_view value);

std::optional<unsigned int> ParseHttpStatusCode(std::string_view header_text);
absl::Status MapHttpErrorStatus(unsigned int status_code, std::string message);
std::string BuildHttpErrorMessage(unsigned int status_code, std::string_view body);

} // namespace isla::server::ai_gateway
