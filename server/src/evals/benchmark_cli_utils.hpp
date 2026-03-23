#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "ai_gateway_startup_config.hpp"

namespace isla::server::evals {

[[nodiscard]] std::optional<std::string_view> ParseFlagValue(std::string_view arg,
                                                             std::string_view prefix);

[[nodiscard]] absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig>
ParseBenchmarkStartupConfig(const std::vector<char*>& gateway_argv,
                            const isla::server::ai_gateway::StartupEnvLookup& env_lookup);

[[nodiscard]] absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig>
ParseBenchmarkStartupConfig(const std::vector<char*>& gateway_argv);

} // namespace isla::server::evals
