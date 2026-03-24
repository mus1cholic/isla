#pragma once

#include <optional>
#include <string>

#include "absl/status/status.h"
#include <nlohmann/json.hpp>

#include "isla/server/evals/eval_types.hpp"

namespace isla::server::evals {

[[nodiscard]] std::string StatusCodeName(absl::StatusCode code);

[[nodiscard]] EvalFailure FailureFromStatus(const absl::Status& status);

[[nodiscard]] nlohmann::ordered_json FailureToJson(const std::optional<EvalFailure>& failure);

} // namespace isla::server::evals
