#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"

namespace isla::server::ai_gateway {

struct ExecutionRuntimeInput {
    std::string system_prompt;
    std::string user_text;
};

struct OpenAiLlmStep {
    std::string step_name;
    std::string system_prompt;
    std::string model;
};

using ExecutionStep = std::variant<OpenAiLlmStep>;

struct LlmCallResult {
    std::string step_name;
    std::string model;
    std::string output_text;
};

using ExecutionStepResult = std::variant<LlmCallResult>;

struct ExecutionResult {
    std::vector<ExecutionStepResult> step_results;
};

struct ExecutionFailure {
    std::size_t failed_step_index = 0;
    std::string step_name;
    std::string code;
    std::string message;
    bool retryable = false;
};

struct ExecutionPlan {
    std::vector<ExecutionStep> steps;
    // TODO(ai-gateway): Let later plan steps consume outputs from earlier steps.
    // TODO(ai-gateway): Support optional parallel execution for independent steps.
};

[[nodiscard]] absl::StatusOr<ExecutionPlan> CreateOpenAiPlan();

} // namespace isla::server::ai_gateway
