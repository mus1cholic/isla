#pragma once

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/openai_reasoning_effort.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

class OpenAiLLMs final {
  public:
    OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model,
               std::shared_ptr<const OpenAiResponsesClient> responses_client = nullptr,
               OpenAiReasoningEffort reasoning_effort = OpenAiReasoningEffort::kNone);

    [[nodiscard]] const std::string& step_name() const;
    [[nodiscard]] absl::Status Validate() const;
    [[nodiscard]] absl::StatusOr<ExecutionStepResult>
    GenerateContent(std::size_t item_index, const ExecutionRuntimeInput& runtime_input) const;

  private:
    [[nodiscard]] absl::Status ValidateInput(const ExecutionRuntimeInput& runtime_input) const;
    [[nodiscard]] absl::StatusOr<std::string>
    GenerateProviderResponse(std::size_t item_index,
                             const ExecutionRuntimeInput& runtime_input) const;

    std::string step_name_;
    // Default step-level system prompt. A non-empty ExecutionRuntimeInput.system_prompt
    // overrides this value for the current request.
    std::string system_prompt_;
    std::string model_;
    OpenAiReasoningEffort reasoning_effort_ = OpenAiReasoningEffort::kNone;
    std::shared_ptr<const OpenAiResponsesClient> responses_client_;
};

} // namespace isla::server::ai_gateway
