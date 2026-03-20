#include "isla/server/ai_gateway_step_registry.hpp"

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/openai_llms.hpp"
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
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

absl::Status resource_exhausted(std::string_view message) {
    return absl::ResourceExhaustedError(std::string(message));
}

std::vector<OpenAiResponsesFunctionTool>
BuildProviderFunctionTools(const isla::server::tools::ToolRegistry& tool_registry) {
    std::vector<OpenAiResponsesFunctionTool> function_tools;
    for (const isla::server::tools::ToolDefinition& tool_definition :
         tool_registry.ListDefinitions()) {
        function_tools.push_back(OpenAiResponsesFunctionTool{
            .name = tool_definition.name,
            .description = tool_definition.description,
            .parameters_json_schema = tool_definition.input_json_schema,
            .strict = true,
        });
    }
    return function_tools;
}

struct OpenAiToolLoopResponse {
    std::string output_text;
    std::vector<OpenAiResponsesOutputItem> output_items;
};

absl::StatusOr<OpenAiToolLoopResponse>
RunOpenAiResponsesRequest(const OpenAiResponsesClient& client,
                          const OpenAiResponsesRequest& request) {
    std::string output_text;
    std::optional<OpenAiResponsesCompletedEvent> completed_event;
    const absl::Status stream_status = client.StreamResponse(
        request,
        [&output_text, &completed_event](const OpenAiResponsesEvent& event) -> absl::Status {
            return std::visit(
                [&output_text, &completed_event](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        output_text.append(concrete_event.text_delta);
                    } else if constexpr (std::is_same_v<Event, OpenAiResponsesCompletedEvent>) {
                        completed_event = concrete_event;
                    }
                    return absl::OkStatus();
                },
                event);
        });
    if (!stream_status.ok()) {
        return stream_status;
    }
    if (!completed_event.has_value()) {
        return failed_precondition("openai responses tool loop completed without a terminal event");
    }
    return OpenAiToolLoopResponse{
        .output_text = std::move(output_text),
        .output_items = std::move(completed_event->output_items),
    };
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
    if (config_.tool_registry != nullptr && config_.openai_client != nullptr &&
        runtime_input.tool_execution_context.has_value()) {
        if (runtime_input.user_text.empty()) {
            return invalid_argument("openai llms input must include user_text");
        }
        const absl::Status client_status = config_.openai_client->Validate();
        if (!client_status.ok()) {
            return client_status;
        }
        const std::string_view effective_system_prompt =
            runtime_input.system_prompt.empty() ? std::string_view(step.system_prompt)
                                                : std::string_view(runtime_input.system_prompt);
        std::vector<OpenAiResponsesInputItem> replay_input_items;
        const std::vector<OpenAiResponsesFunctionTool> function_tools =
            BuildProviderFunctionTools(*config_.tool_registry);
        bool include_initial_user_text = true;
        for (std::size_t round = 0; round < kMaxToolRounds; ++round) {
            const absl::StatusOr<OpenAiToolLoopResponse> response = RunOpenAiResponsesRequest(
                *config_.openai_client,
                OpenAiResponsesRequest{
                    .model = effective_model,
                    .system_prompt = std::string(effective_system_prompt),
                    .user_text =
                        include_initial_user_text ? runtime_input.user_text : std::string(),
                    .input_items = replay_input_items,
                    .function_tools = function_tools,
                    .parallel_tool_calls = false,
                    .reasoning_effort = step.reasoning_effort,
                    .telemetry_context = runtime_input.telemetry_context,
                });
            if (!response.ok()) {
                return response.status();
            }
            include_initial_user_text = false;

            std::vector<isla::server::tools::ToolCall> tool_calls;
            tool_calls.reserve(response->output_items.size());
            for (const OpenAiResponsesOutputItem& output_item : response->output_items) {
                replay_input_items.push_back(OpenAiResponsesRawInputItem{
                    .raw_json = output_item.raw_json,
                });
                if (output_item.type != "function_call") {
                    continue;
                }
                if (!output_item.call_id.has_value() || !output_item.name.has_value() ||
                    !output_item.arguments_json.has_value()) {
                    return failed_precondition(
                        "openai responses function_call item must include call_id, name, and "
                        "arguments");
                }
                tool_calls.push_back(isla::server::tools::ToolCall{
                    .call_id = *output_item.call_id,
                    .name = *output_item.name,
                    .arguments_json = *output_item.arguments_json,
                });
            }
            if (tool_calls.empty()) {
                return ExecutionStepResult(LlmCallResult{
                    .step_name = step.step_name,
                    .model = effective_model,
                    .output_text = response->output_text,
                });
            }

            for (const isla::server::tools::ToolCall& tool_call : tool_calls) {
                const absl::StatusOr<isla::server::tools::ToolResult> tool_result =
                    config_.tool_registry->Execute(*runtime_input.tool_execution_context,
                                                   tool_call);
                if (!tool_result.ok()) {
                    return tool_result.status();
                }
                replay_input_items.push_back(OpenAiResponsesFunctionCallOutputInputItem{
                    .call_id = tool_result->call_id,
                    .output = tool_result->output_text,
                });
            }
        }
        return resource_exhausted("openai tool loop exceeded maximum rounds");
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
