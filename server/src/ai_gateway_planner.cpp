#include "isla/server/ai_gateway_planner.hpp"

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::ai_gateway {
namespace {

constexpr std::string_view kMainStepName = "main";

} // namespace

absl::StatusOr<ExecutionPlan> CreateOpenAiPlan(OpenAiReasoningEffort reasoning_effort) {
    const absl::StatusOr<std::string> system_prompt = isla::server::memory::LoadSystemPrompt();
    if (!system_prompt.ok()) {
        return system_prompt.status();
    }

    ExecutionPlan plan;
    plan.steps.emplace_back(OpenAiLlmStep{
        .step_name = std::string(kMainStepName),
        .system_prompt = *system_prompt,
        .model = std::string(kDefaultMainLlmModel),
        .reasoning_effort = reasoning_effort,
    });
    return plan;
}

} // namespace isla::server::ai_gateway
