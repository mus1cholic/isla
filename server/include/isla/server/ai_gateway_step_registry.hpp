#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

struct GatewayStepRegistryConfig {
    GatewayLlmRuntimeConfig llm_runtime_config;
    OpenAiResponsesClientConfig openai_config;
    std::shared_ptr<const OpenAiResponsesClient> openai_client;
    std::shared_ptr<const isla::server::LlmClient> llm_client;
};

class GatewayStepRegistry final {
  public:
    explicit GatewayStepRegistry(GatewayStepRegistryConfig config = {});

    [[nodiscard]] std::string StepName(const ExecutionStep& step) const;
    [[nodiscard]] absl::StatusOr<ExecutionStepResult>
    ExecuteStep(std::size_t step_index, const ExecutionStep& step,
                const ExecutionRuntimeInput& runtime_input) const;

  private:
    [[nodiscard]] std::string StepName(const OpenAiLlmStep& step) const;
    [[nodiscard]] absl::StatusOr<ExecutionStepResult>
    ExecuteStep(std::size_t step_index, const OpenAiLlmStep& step,
                const ExecutionRuntimeInput& runtime_input) const;

    GatewayStepRegistryConfig config_;
};

} // namespace isla::server::ai_gateway
