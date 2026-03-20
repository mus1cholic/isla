#pragma once

#include <filesystem>

#include "absl/status/status.h"
#include <nlohmann/json_fwd.hpp>

#include "isla/server/evals/eval_types.hpp"

namespace isla::server::evals {

[[nodiscard]] nlohmann::json EvalConversationMessageToJson(const EvalConversationMessage& message);
[[nodiscard]] nlohmann::json EvalInputToJson(const EvalInput& input);
[[nodiscard]] nlohmann::json EvalCaseToJson(const EvalCase& eval_case);
[[nodiscard]] nlohmann::json EvalArtifactsToJson(const EvalArtifacts& artifacts);

[[nodiscard]] absl::Status WriteJsonFile(const std::filesystem::path& path,
                                         const nlohmann::json& payload);
[[nodiscard]] absl::Status WriteJsonFile(const std::filesystem::path& path,
                                         const nlohmann::ordered_json& payload);

} // namespace isla::server::evals
