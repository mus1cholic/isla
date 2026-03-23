#include "isla/server/ai_gateway_step_registry.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/openai_llms.hpp"
#include "isla/server/openai_reasoning_effort_utils.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {
namespace {

constexpr std::string_view kMainStepName = "main";
constexpr std::size_t kMaxToolRounds = 8;

std::string ResolveModelForStep(const GatewayLlmRuntimeConfig& runtime_config,
                                const OpenAiLlmStep& step) {
    if (step.step_name == kMainStepName && !runtime_config.main_model.empty()) {
        return runtime_config.main_model;
    }
    return step.model;
}

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(message);
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(message);
}

absl::Status resource_exhausted(std::string_view message) {
    return absl::ResourceExhaustedError(message);
}

absl::StatusOr<isla::server::LlmReasoningEffort>
ToLlmReasoningEffort(OpenAiReasoningEffort effort) {
    if (const std::optional<isla::server::LlmReasoningEffort> mapped =
            TryOpenAiReasoningEffortToLlmReasoningEffort(effort);
        mapped.has_value()) {
        return *mapped;
    }
    return invalid_argument("openai llm step reasoning_effort is invalid");
}

std::vector<isla::server::LlmFunctionTool>
BuildFunctionTools(const isla::server::tools::ToolRegistry& tool_registry) {
    std::vector<isla::server::LlmFunctionTool> function_tools;
    for (const isla::server::tools::ToolDefinition& tool_definition :
         tool_registry.ListDefinitions()) {
        function_tools.push_back(isla::server::LlmFunctionTool{
            .name = tool_definition.name,
            .description = tool_definition.description,
            .parameters_json_schema = tool_definition.input_json_schema,
            .strict = true,
        });
    }
    return function_tools;
}

} // namespace

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
    const std::string effective_model = ResolveModelForStep(config_.llm_runtime_config, step);
    VLOG(1) << "AI gateway step registry dispatching openai llm step_index=" << step_index
            << " step_name='" << SanitizeForLog(step.step_name) << "' model='"
            << SanitizeForLog(effective_model) << "'";
    ScopedTelemetryPhase step_phase(runtime_input.telemetry_context, telemetry::kPhaseExecutorStep);
    if (config_.tool_registry != nullptr && config_.llm_client != nullptr &&
        config_.llm_client->SupportsToolCalling() &&
        runtime_input.tool_execution_context.has_value()) {
        if (runtime_input.user_text.empty()) {
            return invalid_argument("llm tool loop input must include user_text");
        }
        absl::Status client_status = config_.llm_client->Validate();
        if (!client_status.ok()) {
            return client_status;
        }
        const std::string_view effective_system_prompt =
            runtime_input.system_prompt.empty() ? std::string_view(step.system_prompt)
                                                : std::string_view(runtime_input.system_prompt);
        const absl::StatusOr<isla::server::LlmReasoningEffort> llm_reasoning_effort =
            ToLlmReasoningEffort(step.reasoning_effort);
        if (!llm_reasoning_effort.ok()) {
            return llm_reasoning_effort.status();
        }
        std::string continuation_token;
        std::vector<isla::server::LlmFunctionCallOutput> tool_outputs;
        const std::vector<isla::server::LlmFunctionTool> function_tools =
            BuildFunctionTools(*config_.tool_registry);
        bool include_initial_user_text = true;
        for (std::size_t round = 0; round < kMaxToolRounds; ++round) {
            const absl::StatusOr<isla::server::LlmToolCallResponse> response =
                config_.llm_client->RunToolCallRound(isla::server::LlmToolCallRequest{
                    .model = effective_model,
                    .system_prompt = std::string(effective_system_prompt),
                    .user_text =
                        include_initial_user_text ? runtime_input.user_text : std::string(),
                    .function_tools =
                        std::span<const isla::server::LlmFunctionTool>(function_tools),
                    .tool_outputs =
                        std::span<const isla::server::LlmFunctionCallOutput>(tool_outputs),
                    .continuation_token = continuation_token,
                    .reasoning_effort = *llm_reasoning_effort,
                    .telemetry_context = runtime_input.telemetry_context,
                });
            if (!response.ok()) {
                return response.status();
            }
            include_initial_user_text = false;
            continuation_token = response->continuation_token;

            std::vector<isla::server::tools::ToolCall> tool_calls;
            tool_calls.reserve(response->tool_calls.size());
            for (const isla::server::LlmFunctionCall& function_call : response->tool_calls) {
                tool_calls.push_back(isla::server::tools::ToolCall{
                    .call_id = function_call.call_id,
                    .name = function_call.name,
                    .arguments_json = function_call.arguments_json,
                });
            }
            if (tool_calls.empty()) {
                return ExecutionStepResult(LlmCallResult{
                    .step_name = step.step_name,
                    .model = effective_model,
                    .output_text = std::move(response->output_text),
                });
            }

            tool_outputs.clear();
            tool_outputs.reserve(tool_calls.size());
            for (const isla::server::tools::ToolCall& tool_call : tool_calls) {
                const absl::StatusOr<isla::server::tools::ToolResult> tool_result =
                    config_.tool_registry->Execute(*runtime_input.tool_execution_context,
                                                   tool_call);
                if (!tool_result.ok()) {
                    return tool_result.status();
                }
                tool_outputs.push_back(isla::server::LlmFunctionCallOutput{
                    .call_id = tool_result->call_id,
                    .output = tool_result->output_text,
                });
            }
        }
        return resource_exhausted("llm tool loop exceeded maximum rounds");
    }
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
