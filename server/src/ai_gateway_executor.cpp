#include "isla/server/ai_gateway_executor.hpp"

#include <string_view>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

ExecutionFailure BuildFailure(std::size_t step_index, std::string_view step_name,
                              const absl::Status& status) {
    return ExecutionFailure{
        .failed_step_index = step_index,
        .step_name = std::string(step_name),
        .code = status.code() == absl::StatusCode::kInvalidArgument ? "invalid_request"
                                                                    : "internal_error",
        .message = std::string(status.message()),
        .retryable = false,
    };
}

} // namespace

GatewayPlanExecutor::GatewayPlanExecutor(GatewayStepRegistryConfig config)
    : step_registry_(std::move(config)) {}

ExecutionOutcome GatewayPlanExecutor::Execute(const ExecutionPlan& plan,
                                              const ExecutionRuntimeInput& runtime_input) const {
    if (plan.steps.empty()) {
        return ExecutionFailure{
            .failed_step_index = 0,
            .step_name = "",
            .code = "invalid_request",
            .message = "execution plan must include at least one step",
            .retryable = false,
        };
    }

    ExecutionResult result;
    result.step_results.reserve(plan.steps.size());
    for (std::size_t step_index = 0; step_index < plan.steps.size(); ++step_index) {
        VLOG(1) << "AI gateway executor running step_index=" << step_index
                << " step_name='"
                << SanitizeForLog(step_registry_.StepName(plan.steps[step_index])) << "'";
        const absl::StatusOr<ExecutionStepResult> step_result =
            step_registry_.ExecuteStep(step_index, plan.steps[step_index], runtime_input);
        if (!step_result.ok()) {
            return BuildFailure(step_index, step_registry_.StepName(plan.steps[step_index]),
                                step_result.status());
        }
        result.step_results.push_back(*step_result);
    }

    return result;
}

} // namespace isla::server::ai_gateway
