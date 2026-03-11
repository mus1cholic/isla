#pragma once

#include <cstddef>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_planner.hpp"

namespace isla::server::ai_gateway {

struct GatewayStepRegistryConfig {
    std::string response_prefix = "stub echo: ";
    OpenAiResponseBuilder response_builder;
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
