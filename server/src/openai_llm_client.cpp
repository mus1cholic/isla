#include "isla/server/openai_llm_client.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "isla/server/openai_reasoning_effort.hpp"
#include "isla/server/openai_responses_client.hpp"
#include <nlohmann/json.hpp>

namespace isla::server {
namespace {

using isla::server::ai_gateway::OpenAiReasoningEffort;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEvent;
using isla::server::ai_gateway::OpenAiResponsesFunctionCallOutputInputItem;
using isla::server::ai_gateway::OpenAiResponsesFunctionTool;
using isla::server::ai_gateway::OpenAiResponsesInputItem;
using isla::server::ai_gateway::OpenAiResponsesMessageInputItem;
using isla::server::ai_gateway::OpenAiResponsesOutputItem;
using isla::server::ai_gateway::OpenAiResponsesRawInputItem;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using nlohmann::json;

template <typename> inline constexpr bool kAlwaysFalse = false;
constexpr std::size_t kMaxToolContinuationBytes = 128U * 1024U;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status resource_exhausted(std::string_view message) {
    return absl::ResourceExhaustedError(std::string(message));
}

absl::StatusOr<OpenAiReasoningEffort> ToOpenAiReasoningEffort(LlmReasoningEffort effort) {
    switch (effort) {
    case LlmReasoningEffort::kNone:
        return OpenAiReasoningEffort::kNone;
    case LlmReasoningEffort::kMinimal:
        return OpenAiReasoningEffort::kMinimal;
    case LlmReasoningEffort::kLow:
        return OpenAiReasoningEffort::kLow;
    case LlmReasoningEffort::kMedium:
        return OpenAiReasoningEffort::kMedium;
    case LlmReasoningEffort::kHigh:
        return OpenAiReasoningEffort::kHigh;
    case LlmReasoningEffort::kXHigh:
        return OpenAiReasoningEffort::kXHigh;
    }
    return invalid_argument("llm client reasoning_effort is invalid");
}

absl::StatusOr<std::vector<OpenAiResponsesFunctionTool>>
ToOpenAiFunctionTools(std::span<const LlmFunctionTool> tools) {
    std::vector<OpenAiResponsesFunctionTool> translated_tools;
    translated_tools.reserve(tools.size());
    for (const LlmFunctionTool& tool : tools) {
        translated_tools.push_back(OpenAiResponsesFunctionTool{
            .name = tool.name,
            .description = tool.description,
            .parameters_json_schema = tool.parameters_json_schema,
            .strict = tool.strict,
        });
    }
    return translated_tools;
}

absl::StatusOr<std::vector<OpenAiResponsesInputItem>>
DeserializeContinuationToken(std::string_view token) {
    if (token.empty()) {
        return std::vector<OpenAiResponsesInputItem>{};
    }
    const json parsed = json::parse(token, nullptr, false);
    if (parsed.is_discarded()) {
        return invalid_argument("llm tool continuation_token must be valid JSON");
    }
    if (!parsed.is_array()) {
        return invalid_argument("llm tool continuation_token must be a JSON array");
    }
    std::vector<OpenAiResponsesInputItem> items;
    items.reserve(parsed.size());
    for (const json& entry : parsed) {
        if (!entry.is_object()) {
            return invalid_argument("llm tool continuation_token entries must be JSON objects");
        }
        const auto type_it = entry.find("type");
        if (type_it == entry.end() || !type_it->is_string()) {
            return invalid_argument(
                "llm tool continuation_token entries must include a string type");
        }
        const std::string type = type_it->get<std::string>();
        if (type == "raw") {
            const auto raw_json_it = entry.find("raw_json");
            if (raw_json_it == entry.end() || !raw_json_it->is_string()) {
                return invalid_argument("llm tool continuation raw entries must include raw_json");
            }
            items.emplace_back(OpenAiResponsesRawInputItem{
                .raw_json = raw_json_it->get<std::string>(),
            });
        } else if (type == "message") {
            const auto role_it = entry.find("role");
            const auto content_it = entry.find("content");
            if (role_it == entry.end() || !role_it->is_string() || content_it == entry.end() ||
                !content_it->is_string()) {
                return invalid_argument(
                    "llm tool continuation message entries must include role and content");
            }
            items.emplace_back(OpenAiResponsesMessageInputItem{
                .role = role_it->get<std::string>(),
                .content = content_it->get<std::string>(),
            });
        } else if (type == "function_call_output") {
            const auto call_id_it = entry.find("call_id");
            const auto output_it = entry.find("output");
            if (call_id_it == entry.end() || !call_id_it->is_string() || output_it == entry.end() ||
                !output_it->is_string()) {
                return invalid_argument("llm tool continuation function_call_output entries must "
                                        "include call_id and output");
            }
            items.emplace_back(OpenAiResponsesFunctionCallOutputInputItem{
                .call_id = call_id_it->get<std::string>(),
                .output = output_it->get<std::string>(),
            });
        } else {
            return invalid_argument("llm tool continuation_token entry type is unsupported");
        }
    }
    return items;
}

std::string SerializeContinuationToken(std::span<const OpenAiResponsesInputItem> items) {
    json serialized = json::array();
    for (const OpenAiResponsesInputItem& item : items) {
        std::visit(
            [&serialized](const auto& concrete_item) {
                using Item = std::decay_t<decltype(concrete_item)>;
                if constexpr (std::is_same_v<Item, OpenAiResponsesRawInputItem>) {
                    serialized.push_back({
                        { "type", "raw" },
                        { "raw_json", concrete_item.raw_json },
                    });
                } else if constexpr (std::is_same_v<Item, OpenAiResponsesMessageInputItem>) {
                    serialized.push_back({
                        { "type", "message" },
                        { "role", concrete_item.role },
                        { "content", concrete_item.content },
                    });
                } else if constexpr (std::is_same_v<Item,
                                                    OpenAiResponsesFunctionCallOutputInputItem>) {
                    serialized.push_back({
                        { "type", "function_call_output" },
                        { "call_id", concrete_item.call_id },
                        { "output", concrete_item.output },
                    });
                } else {
                    static_assert(kAlwaysFalse<Item>, "SerializeContinuationToken must handle all "
                                                      "OpenAiResponsesInputItem alternatives");
                }
            },
            item);
    }
    return serialized.dump();
}

class OpenAiLlmClient final : public LlmClient {
  public:
    explicit OpenAiLlmClient(std::shared_ptr<const OpenAiResponsesClient> responses_client)
        : responses_client_(std::move(responses_client)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return responses_client_->Validate();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return responses_client_->WarmUp();
    }

    [[nodiscard]] bool SupportsToolCalling() const override {
        return true;
    }

    [[nodiscard]] absl::Status StreamResponse(const LlmRequest& request,
                                              const LlmEventCallback& on_event) const override {
        const absl::StatusOr<OpenAiReasoningEffort> reasoning_effort =
            ToOpenAiReasoningEffort(request.reasoning_effort);
        if (!reasoning_effort.ok()) {
            return reasoning_effort.status();
        }

        return responses_client_->StreamResponse(
            OpenAiResponsesRequest{
                .model = request.model,
                .system_prompt = request.system_prompt,
                .user_text = request.user_text,
                .reasoning_effort = *reasoning_effort,
                .telemetry_context = request.telemetry_context,
            },
            [&on_event](const OpenAiResponsesEvent& event) -> absl::Status {
                return std::visit(
                    [&on_event](const auto& concrete_event) -> absl::Status {
                        using Event = std::decay_t<decltype(concrete_event)>;
                        if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                            return on_event(LlmTextDeltaEvent{
                                .text_delta = concrete_event.text_delta,
                            });
                        }
                        if constexpr (std::is_same_v<Event, OpenAiResponsesCompletedEvent>) {
                            return on_event(LlmCompletedEvent{
                                .response_id = concrete_event.response_id,
                            });
                        }
                        return absl::OkStatus();
                    },
                    event);
            });
    }

    [[nodiscard]] absl::StatusOr<LlmToolCallResponse>
    RunToolCallRound(const LlmToolCallRequest& request) const override {
        const absl::StatusOr<OpenAiReasoningEffort> reasoning_effort =
            ToOpenAiReasoningEffort(request.reasoning_effort);
        if (!reasoning_effort.ok()) {
            return reasoning_effort.status();
        }
        if (request.continuation_token.size() > kMaxToolContinuationBytes) {
            return resource_exhausted("openai tool call continuation_token exceeds maximum length");
        }
        const absl::StatusOr<std::vector<OpenAiResponsesFunctionTool>> function_tools =
            ToOpenAiFunctionTools(request.function_tools);
        if (!function_tools.ok()) {
            return function_tools.status();
        }
        absl::StatusOr<std::vector<OpenAiResponsesInputItem>> replay_input_items =
            DeserializeContinuationToken(request.continuation_token);
        if (!replay_input_items.ok()) {
            return replay_input_items.status();
        }
        replay_input_items->reserve(replay_input_items->size() + request.tool_outputs.size());
        for (const LlmFunctionCallOutput& tool_output : request.tool_outputs) {
            replay_input_items->push_back(OpenAiResponsesFunctionCallOutputInputItem{
                .call_id = tool_output.call_id,
                .output = tool_output.output,
            });
        }

        std::string output_text;
        std::optional<OpenAiResponsesCompletedEvent> completed_event;
        absl::Status stream_status = responses_client_->StreamResponse(
            OpenAiResponsesRequest{
                .model = request.model,
                .system_prompt = request.system_prompt,
                .user_text = request.user_text,
                .input_items = std::span<const OpenAiResponsesInputItem>(*replay_input_items),
                .function_tools = std::span<const OpenAiResponsesFunctionTool>(*function_tools),
                .parallel_tool_calls = false,
                .reasoning_effort = *reasoning_effort,
                .telemetry_context = request.telemetry_context,
            },
            [&output_text, &completed_event](const OpenAiResponsesEvent& event) -> absl::Status {
                return std::visit(
                    [&output_text, &completed_event](const auto& concrete_event) -> absl::Status {
                        using Event = std::decay_t<decltype(concrete_event)>;
                        if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                            if (concrete_event.text_delta.empty()) {
                                return absl::OkStatus();
                            }
                            if (output_text.size() + concrete_event.text_delta.size() >
                                ai_gateway::kMaxTextOutputBytes) {
                                return resource_exhausted(
                                    "openai tool call round output exceeds maximum length");
                            }
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
            return absl::FailedPreconditionError(
                "openai tool call round completed without a terminal event");
        }

        std::vector<LlmFunctionCall> tool_calls;
        tool_calls.reserve(completed_event->output_items.size());
        for (const OpenAiResponsesOutputItem& output_item : completed_event->output_items) {
            replay_input_items->push_back(OpenAiResponsesRawInputItem{
                .raw_json = output_item.raw_json,
            });
            if (output_item.type != "function_call") {
                continue;
            }
            if (!output_item.call_id.has_value() || !output_item.name.has_value() ||
                !output_item.arguments_json.has_value()) {
                return absl::FailedPreconditionError(
                    "openai function_call item must include call_id, name, and arguments");
            }
            tool_calls.push_back(LlmFunctionCall{
                .call_id = *output_item.call_id,
                .name = *output_item.name,
                .arguments_json = *output_item.arguments_json,
            });
        }

        const std::string continuation_token = SerializeContinuationToken(
            std::span<const OpenAiResponsesInputItem>(*replay_input_items));
        if (continuation_token.size() > kMaxToolContinuationBytes) {
            return resource_exhausted("openai tool call continuation_token exceeds maximum length");
        }

        return LlmToolCallResponse{
            .output_text = std::move(output_text),
            .tool_calls = std::move(tool_calls),
            .continuation_token = continuation_token,
            .response_id = completed_event->response_id,
        };
    }

  private:
    std::shared_ptr<const OpenAiResponsesClient> responses_client_;
};

} // namespace

absl::StatusOr<std::shared_ptr<const LlmClient>> CreateOpenAiLlmClient(
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> responses_client) {
    if (responses_client == nullptr) {
        return invalid_argument("OpenAiLlmClient requires a non-null responses client");
    }
    return std::shared_ptr<const LlmClient>(
        std::make_shared<OpenAiLlmClient>(std::move(responses_client)));
}

} // namespace isla::server
