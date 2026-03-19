#include "isla/server/ai_gateway_step_registry.hpp"

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/openai_llms.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

GatewayStepRegistry::GatewayStepRegistry(GatewayStepRegistryConfig config)
    : config_(std::move(config)) {
    if (config_.openai_client == nullptr && config_.openai_config.enabled) {
        config_.openai_client = CreateOpenAiResponsesClient(config_.openai_config);
    }
    if (config_.llm_client == nullptr && config_.openai_client != nullptr) {
        const absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> llm_client =
            isla::server::CreateOpenAiLlmClient(config_.openai_client);
        if (llm_client.ok()) {
            config_.llm_client = *llm_client;
        } else {
            LOG(WARNING) << "AI gateway step registry could not adapt OpenAI responses client to "
                            "a generic llm client detail='"
                         << SanitizeForLog(llm_client.status().message()) << "'";
        }
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
    const std::string effective_model = config_.llm_runtime_config.main_model.empty()
                                            ? step.model
                                            : config_.llm_runtime_config.main_model;
    VLOG(1) << "AI gateway step registry dispatching openai llm step_index=" << step_index
            << " step_name='" << SanitizeForLog(step.step_name) << "' model='"
            << SanitizeForLog(effective_model) << "'";
    ScopedTelemetryPhase step_phase(runtime_input.telemetry_context, telemetry::kPhaseExecutorStep);
    OpenAiLLMs openai_llms(step.step_name, step.system_prompt, effective_model, config_.llm_client,
                           step.reasoning_effort);
    return openai_llms.GenerateContent(step_index, runtime_input);
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
