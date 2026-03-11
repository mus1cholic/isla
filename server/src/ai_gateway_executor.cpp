#include "isla/server/ai_gateway_executor.hpp"

#include <string_view>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

struct PublicFailureMapping {
    std::string_view code;
    std::string_view message;
    bool retryable = false;
};

PublicFailureMapping MapPublicFailure(const absl::Status& status) {
    switch (status.code()) {
    case absl::StatusCode::kInvalidArgument:
        return PublicFailureMapping{
            .code = "bad_request",
            .message = "execution step rejected the request",
            .retryable = false,
        };
    case absl::StatusCode::kUnauthenticated:
        return PublicFailureMapping{
            .code = "authentication_error",
            .message = "upstream authentication failed",
            .retryable = false,
        };
    case absl::StatusCode::kPermissionDenied:
        return PublicFailureMapping{
            .code = "permission_denied",
            .message = "upstream request was not permitted",
            .retryable = false,
        };
    case absl::StatusCode::kResourceExhausted:
        return PublicFailureMapping{
            .code = "response_too_large",
            .message = "execution step produced too much output",
            .retryable = false,
        };
    case absl::StatusCode::kDeadlineExceeded:
        return PublicFailureMapping{
            .code = "upstream_timeout",
            .message = "upstream request timed out",
            .retryable = true,
        };
    case absl::StatusCode::kUnavailable:
        return PublicFailureMapping{
            .code = "service_unavailable",
            .message = "upstream service unavailable",
            .retryable = true,
        };
    default:
        return PublicFailureMapping{
            .code = "internal_error",
            .message = "execution step failed",
            .retryable = false,
        };
    }
}

ExecutionFailure BuildFailure(std::size_t step_index, std::string_view step_name,
                              const absl::Status& status) {
    const PublicFailureMapping mapping = MapPublicFailure(status);
    LOG(ERROR) << "AI gateway executor step failed step_index=" << step_index << " step_name='"
               << SanitizeForLog(step_name) << "' status_code=" << static_cast<int>(status.code())
               << " public_code='" << mapping.code << "' retryable=" << mapping.retryable
               << " detail='" << SanitizeForLog(status.message()) << "'";
    return ExecutionFailure{
        .failed_step_index = step_index,
        .step_name = std::string(step_name),
        .code = std::string(mapping.code),
        .message = std::string(mapping.message),
        .retryable = mapping.retryable,
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
            .code = "bad_request",
            .message = "execution plan must include at least one step",
            .retryable = false,
        };
    }

    ExecutionResult result;
    result.step_results.reserve(plan.steps.size());
    for (std::size_t step_index = 0; step_index < plan.steps.size(); ++step_index) {
        VLOG(1) << "AI gateway executor running step_index=" << step_index << " step_name='"
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
