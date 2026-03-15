#include "isla/server/ai_gateway_planner.hpp"

#include "absl/status/statusor.h"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::ai_gateway {
namespace {

constexpr std::string_view kMainStepName = "main";
// constexpr std::string_view kMainModel = "gpt-5.4";
constexpr std::string_view kMainModel = "gpt-5-mini-2025-08-07";

} // namespace

absl::StatusOr<ExecutionPlan> CreateOpenAiPlan() {
    const absl::StatusOr<std::string> system_prompt = isla::server::memory::LoadSystemPrompt();
    if (!system_prompt.ok()) {
        return system_prompt.status();
    }

    ExecutionPlan plan;
    plan.steps.emplace_back(OpenAiLlmStep{
        .step_name = std::string(kMainStepName),
        .system_prompt = *system_prompt,
        .model = std::string(kMainModel),
        .reasoning_effort = OpenAiReasoningEffort::kMinimal,
    });
    return plan;
}

} // namespace isla::server::ai_gateway
