#pragma once

#include <variant>

#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/ai_gateway_step_registry.hpp"

namespace isla::server::ai_gateway {

using ExecutionOutcome = std::variant<ExecutionResult, ExecutionFailure>;

class GatewayPlanExecutor final {
  public:
    explicit GatewayPlanExecutor(GatewayStepRegistryConfig config = {});

    [[nodiscard]] ExecutionOutcome
    Execute(const ExecutionPlan& plan, const ExecutionRuntimeInput& runtime_input) const;

  private:
    GatewayStepRegistry step_registry_;
};

} // namespace isla::server::ai_gateway
