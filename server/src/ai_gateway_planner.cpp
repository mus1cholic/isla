#include "isla/server/ai_gateway_planner.hpp"

#include "absl/status/statusor.h"

namespace isla::server::ai_gateway {
namespace {

constexpr std::string_view kMainStepName = "main";
constexpr std::string_view kMainModel = "gpt-5.2";

} // namespace

absl::StatusOr<ExecutionPlan> CreateFakeOpenAiPlan() {
    ExecutionPlan plan;
    plan.steps.emplace_back(OpenAiLlmStep{
        .step_name = std::string(kMainStepName),
        .system_prompt = "",
        .model = std::string(kMainModel),
    });
    return plan;
}

} // namespace isla::server::ai_gateway
