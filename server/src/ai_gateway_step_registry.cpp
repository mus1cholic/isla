#include "isla/server/ai_gateway_step_registry.hpp"

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_llms.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

GatewayStepRegistry::GatewayStepRegistry(GatewayStepRegistryConfig config)
    : config_(std::move(config)) {
    if (config_.openai_client == nullptr && config_.openai_config.enabled) {
        config_.openai_client = CreateOpenAiResponsesClient(config_.openai_config);
    }
}

std::string GatewayStepRegistry::StepName(const OpenAiLlmStep& step) const {
    return step.step_name;
}

std::string GatewayStepRegistry::StepName(const ExecutionStep& step) const {
    return std::visit([this](const auto& concrete_step) { return StepName(concrete_step); }, step);
}

absl::StatusOr<ExecutionStepResult>
GatewayStepRegistry::ExecuteStep(std::size_t step_index, const OpenAiLlmStep& step,
                                 const ExecutionRuntimeInput& runtime_input) const {
    VLOG(1) << "AI gateway step registry dispatching openai llm step_index=" << step_index
            << " step_name='" << SanitizeForLog(step.step_name) << "' model='"
            << SanitizeForLog(step.model) << "'";
    OpenAiLLMs openai_llms(step.step_name, step.system_prompt, step.model, config_.openai_client);
    return openai_llms.GenerateContent(step_index, runtime_input.user_text);
}

absl::StatusOr<ExecutionStepResult>
GatewayStepRegistry::ExecuteStep(std::size_t step_index, const ExecutionStep& step,
                                 const ExecutionRuntimeInput& runtime_input) const {
    return std::visit(
        [this, step_index,
         &runtime_input](const auto& concrete_step) -> absl::StatusOr<ExecutionStepResult> {
            return ExecuteStep(step_index, concrete_step, runtime_input);
        },
        step);
}

} // namespace isla::server::ai_gateway
