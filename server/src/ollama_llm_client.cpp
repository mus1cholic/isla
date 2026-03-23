#include "isla/server/ollama_llm_client.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_json_client.hpp"
#include "isla/server/ai_gateway_session_handler.hpp"

namespace isla::server {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxToolContinuationBytes = 128U * 1024U;

struct OllamaPendingToolCall {
    std::string call_id;
    std::string tool_name;
};

struct OllamaToolContinuationState {
    std::vector<json> messages;
    std::vector<OllamaPendingToolCall> pending_tool_calls;
};

struct ParsedOllamaChatResponse {
    std::string output_text;
    std::string response_id;
};

struct ParsedOllamaToolRoundResponse {
    std::string output_text;
    std::string response_id;
    json assistant_message;
    std::vector<LlmFunctionCall> tool_calls;
    std::vector<OllamaPendingToolCall> pending_tool_calls;
};

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

absl::Status resource_exhausted(std::string_view message) {
    return absl::ResourceExhaustedError(std::string(message));
}

absl::Status MapHttpFailure(unsigned int status_code, std::string message) {
    switch (status_code) {
    case 400:
    case 404:
    case 409:
    case 422:
        return absl::InvalidArgumentError(std::move(message));
    case 401:
        return absl::UnauthenticatedError(std::move(message));
    case 403:
        return absl::PermissionDeniedError(std::move(message));
    case 408:
    case 504:
        return absl::DeadlineExceededError(std::move(message));
    case 429:
    case 500:
    case 502:
    case 503:
        return absl::UnavailableError(std::move(message));
    default:
        return absl::InternalError(std::move(message));
    }
}

absl::StatusOr<ParsedHttpUrl> ParseOllamaBaseUrl(std::string_view base_url) {
    if (base_url.empty()) {
        return invalid_argument("ollama base_url must not be empty");
    }
    return ParseHttpUrl(base_url, "ollama base_url");
}

absl::StatusOr<ParsedHttpUrl>
ValidateOllamaLlmClientConfigForCreation(const OllamaLlmClientConfig& config) {
    if (!config.enabled) {
        return invalid_argument("ollama llm client is disabled");
    }
    if (config.request_timeout <= std::chrono::milliseconds::zero()) {
        return invalid_argument("ollama request_timeout must be positive");
    }
    return ParseOllamaBaseUrl(config.base_url);
}

absl::StatusOr<bool> ResolveThinkFlag(LlmReasoningEffort effort) {
    switch (effort) {
    case LlmReasoningEffort::kNone:
        return false;
    case LlmReasoningEffort::kMinimal:
    case LlmReasoningEffort::kLow:
    case LlmReasoningEffort::kMedium:
    case LlmReasoningEffort::kHigh:
    case LlmReasoningEffort::kXHigh:
        return true;
    }
    return invalid_argument("ollama llm client reasoning_effort is invalid");
}

std::vector<std::pair<std::string, std::string>> BuildHeaders(const OllamaLlmClientConfig& config) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (config.api_key.has_value() && !config.api_key->empty()) {
        headers.emplace_back("Authorization", "Bearer " + *config.api_key);
    }
    return headers;
}

std::string ExtractErrorMessage(const HttpResponse& response) {
    const std::string fallback =
        "ollama request failed with status " + std::to_string(response.status_code);
    const json parsed = json::parse(response.body, nullptr, false);
    if (!parsed.is_object()) {
        return fallback;
    }
    if (const auto it = parsed.find("error"); it != parsed.end() && it->is_string()) {
        return it->get<std::string>();
    }
    if (const auto it = parsed.find("message"); it != parsed.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

absl::StatusOr<json> ParseJsonObject(std::string_view text, std::string_view label) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return failed_precondition(std::string(label) + " must be valid JSON");
    }
    if (!parsed.is_object()) {
        return failed_precondition(std::string(label) + " must be a JSON object");
    }
    return parsed;
}

absl::StatusOr<json> ParseJsonObjectForInput(std::string_view text, std::string_view label) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return invalid_argument(std::string(label) + " must be valid JSON");
    }
    if (!parsed.is_object()) {
        return invalid_argument(std::string(label) + " must be a JSON object");
    }
    return parsed;
}

absl::StatusOr<std::vector<json>> BuildTextMessages(std::string_view system_prompt,
                                                    std::string_view user_text) {
    std::vector<json> messages;
    if (!system_prompt.empty()) {
        messages.push_back(json{
            { "role", "system" },
            { "content", std::string(system_prompt) },
        });
    }
    messages.push_back(json{
        { "role", "user" },
        { "content", std::string(user_text) },
    });
    return messages;
}

absl::StatusOr<json> ParseToolParametersJsonSchema(std::string_view schema) {
    const json parsed = json::parse(schema, nullptr, false);
    if (parsed.is_discarded()) {
        return invalid_argument("ollama function tool parameters_json_schema must be valid JSON");
    }
    if (!parsed.is_object()) {
        return invalid_argument(
            "ollama function tool parameters_json_schema must be a JSON object");
    }
    return parsed;
}

absl::StatusOr<json> ToOllamaToolsJson(std::span<const LlmFunctionTool> tools) {
    json translated_tools = json::array();
    for (const LlmFunctionTool& tool : tools) {
        if (tool.name.empty()) {
            return invalid_argument("ollama function tool name must not be empty");
        }
        const absl::StatusOr<json> parameters =
            ParseToolParametersJsonSchema(tool.parameters_json_schema);
        if (!parameters.ok()) {
            return parameters.status();
        }
        translated_tools.push_back(json{
            { "type", "function" },
            { "function",
              {
                  { "name", tool.name },
                  { "description", tool.description },
                  { "parameters", *parameters },
              } },
        });
    }
    return translated_tools;
}

absl::StatusOr<OllamaToolContinuationState>
DeserializeToolContinuationToken(std::string_view continuation_token) {
    if (continuation_token.empty()) {
        return OllamaToolContinuationState{};
    }

    const absl::StatusOr<json> parsed =
        ParseJsonObjectForInput(continuation_token, "ollama tool continuation_token");
    if (!parsed.ok()) {
        return parsed.status();
    }

    OllamaToolContinuationState state;
    if (const auto messages_it = parsed->find("messages"); messages_it != parsed->end()) {
        if (!messages_it->is_array()) {
            return invalid_argument("ollama tool continuation_token messages must be a JSON array");
        }
        state.messages.reserve(messages_it->size());
        for (const json& message : *messages_it) {
            if (!message.is_object()) {
                return invalid_argument(
                    "ollama tool continuation_token messages must be JSON objects");
            }
            state.messages.push_back(message);
        }
    }

    if (const auto pending_it = parsed->find("pending_tool_calls"); pending_it != parsed->end()) {
        if (!pending_it->is_array()) {
            return invalid_argument(
                "ollama tool continuation_token pending_tool_calls must be a JSON array");
        }
        state.pending_tool_calls.reserve(pending_it->size());
        for (const json& pending : *pending_it) {
            if (!pending.is_object()) {
                return invalid_argument(
                    "ollama tool continuation_token pending_tool_calls must be JSON objects");
            }
            const auto call_id_it = pending.find("call_id");
            const auto tool_name_it = pending.find("tool_name");
            if (call_id_it == pending.end() || !call_id_it->is_string() ||
                tool_name_it == pending.end() || !tool_name_it->is_string()) {
                return invalid_argument("ollama tool continuation pending_tool_calls must include "
                                        "string call_id and tool_name");
            }
            state.pending_tool_calls.push_back(OllamaPendingToolCall{
                .call_id = call_id_it->get<std::string>(),
                .tool_name = tool_name_it->get<std::string>(),
            });
        }
    }

    return state;
}

std::string SerializeToolContinuationToken(const OllamaToolContinuationState& state) {
    if (state.messages.empty() && state.pending_tool_calls.empty()) {
        return "";
    }

    json serialized = {
        { "messages", json::array() },
        { "pending_tool_calls", json::array() },
    };
    for (const json& message : state.messages) {
        serialized["messages"].push_back(message);
    }
    for (const OllamaPendingToolCall& pending_tool_call : state.pending_tool_calls) {
        serialized["pending_tool_calls"].push_back(json{
            { "call_id", pending_tool_call.call_id },
            { "tool_name", pending_tool_call.tool_name },
        });
    }
    return serialized.dump();
}

absl::StatusOr<std::vector<json>>
BuildToolMessagesForRequest(std::string_view user_text,
                            std::span<const LlmFunctionCallOutput> tool_outputs,
                            const OllamaToolContinuationState& state) {
    if (state.pending_tool_calls.empty() && !tool_outputs.empty()) {
        return invalid_argument("ollama tool outputs require pending tool calls");
    }
    if (!state.pending_tool_calls.empty() &&
        tool_outputs.size() != state.pending_tool_calls.size()) {
        return invalid_argument("ollama tool outputs must cover all pending tool calls");
    }

    std::vector<json> messages;
    messages.reserve(state.messages.size() + (user_text.empty() ? 0U : 1U) + tool_outputs.size());
    for (const json& message : state.messages) {
        messages.push_back(message);
    }
    if (!user_text.empty()) {
        messages.push_back(json{
            { "role", "user" },
            { "content", std::string(user_text) },
        });
    }
    if (messages.empty()) {
        return invalid_argument(
            "ollama tool call request must include user_text or continuation_token");
    }

    for (const LlmFunctionCallOutput& tool_output : tool_outputs) {
        const auto pending_it =
            std::find_if(state.pending_tool_calls.begin(), state.pending_tool_calls.end(),
                         [&tool_output](const OllamaPendingToolCall& pending_tool_call) {
                             return pending_tool_call.call_id == tool_output.call_id;
                         });
        if (pending_it == state.pending_tool_calls.end()) {
            return invalid_argument("ollama tool output must match a pending tool call");
        }
        messages.push_back(json{
            { "role", "tool" },
            { "tool_name", pending_it->tool_name },
            { "content", tool_output.output },
        });
    }

    return messages;
}

absl::StatusOr<ParsedOllamaChatResponse> ParseChatResponse(const HttpResponse& response) {
    const absl::StatusOr<json> parsed = ParseJsonObject(response.body, "ollama chat response");
    if (!parsed.ok()) {
        return parsed.status();
    }

    const auto message_it = parsed->find("message");
    if (message_it == parsed->end() || !message_it->is_object()) {
        return failed_precondition("ollama chat response must include a message object");
    }
    const json& message = *message_it;

    if (const auto tool_calls_it = message.find("tool_calls");
        tool_calls_it != message.end() && tool_calls_it->is_array() && !tool_calls_it->empty()) {
        return failed_precondition(
            "ollama StreamResponse received tool calls; use RunToolCallRound instead");
    }

    std::string content;
    if (const auto content_it = message.find("content");
        content_it != message.end() && !content_it->is_null()) {
        if (!content_it->is_string()) {
            return failed_precondition("ollama chat response message.content must be a string");
        }
        content = content_it->get<std::string>();
    }

    std::string response_id;
    if (const auto created_at_it = parsed->find("created_at");
        created_at_it != parsed->end() && created_at_it->is_string()) {
        response_id = created_at_it->get<std::string>();
    }

    return ParsedOllamaChatResponse{
        .output_text = std::move(content),
        .response_id = std::move(response_id),
    };
}

absl::StatusOr<ParsedOllamaToolRoundResponse> ParseToolRoundResponse(const HttpResponse& response) {
    const absl::StatusOr<json> parsed =
        ParseJsonObject(response.body, "ollama tool call round response");
    if (!parsed.ok()) {
        return parsed.status();
    }

    const auto message_it = parsed->find("message");
    if (message_it == parsed->end() || !message_it->is_object()) {
        return failed_precondition("ollama tool call round response must include a message object");
    }
    const json& message = *message_it;

    const auto role_it = message.find("role");
    if (role_it == message.end() || !role_it->is_string()) {
        return failed_precondition("ollama tool call round response message.role must be a string");
    }

    json assistant_message = {
        { "role", role_it->get<std::string>() },
    };

    std::string output_text;
    if (const auto content_it = message.find("content");
        content_it != message.end() && !content_it->is_null()) {
        if (!content_it->is_string()) {
            return failed_precondition(
                "ollama tool call round response message.content must be a string");
        }
        output_text = content_it->get<std::string>();
        assistant_message["content"] = output_text;
    }
    if (output_text.size() > ai_gateway::kMaxTextOutputBytes) {
        return resource_exhausted("ollama tool call round output exceeds maximum length");
    }

    if (const auto thinking_it = message.find("thinking");
        thinking_it != message.end() && !thinking_it->is_null()) {
        if (!thinking_it->is_string()) {
            return failed_precondition(
                "ollama tool call round response message.thinking must be a string");
        }
        assistant_message["thinking"] = thinking_it->get<std::string>();
    }

    std::vector<LlmFunctionCall> tool_calls;
    std::vector<OllamaPendingToolCall> pending_tool_calls;
    if (const auto tool_calls_it = message.find("tool_calls");
        tool_calls_it != message.end() && !tool_calls_it->is_null()) {
        if (!tool_calls_it->is_array()) {
            return failed_precondition(
                "ollama tool call round response message.tool_calls must be a JSON array");
        }
        json serialized_tool_calls = json::array();
        tool_calls.reserve(tool_calls_it->size());
        pending_tool_calls.reserve(tool_calls_it->size());
        for (std::size_t index = 0; index < tool_calls_it->size(); ++index) {
            const json& tool_call = (*tool_calls_it)[index];
            if (!tool_call.is_object()) {
                return failed_precondition(
                    "ollama tool call round response tool_calls entries must be JSON objects");
            }
            const auto function_it = tool_call.find("function");
            if (function_it == tool_call.end() || !function_it->is_object()) {
                return failed_precondition("ollama tool call round response tool_calls entries "
                                           "must include a function object");
            }
            const json& function = *function_it;
            const auto name_it = function.find("name");
            const auto arguments_it = function.find("arguments");
            if (name_it == function.end() || !name_it->is_string() ||
                arguments_it == function.end() || !arguments_it->is_object()) {
                return failed_precondition("ollama tool call round response functions must include "
                                           "string name and object arguments");
            }
            const std::string name = name_it->get<std::string>();
            const std::string call_id = "ollama_tool_call_" + std::to_string(index);
            tool_calls.push_back(LlmFunctionCall{
                .call_id = call_id,
                .name = name,
                .arguments_json = arguments_it->dump(),
            });
            pending_tool_calls.push_back(OllamaPendingToolCall{
                .call_id = call_id,
                .tool_name = name,
            });
            serialized_tool_calls.push_back(json{
                { "type", "function" },
                { "function",
                  {
                      { "index", index },
                      { "name", name },
                      { "arguments", *arguments_it },
                  } },
            });
        }
        if (!serialized_tool_calls.empty()) {
            assistant_message["tool_calls"] = std::move(serialized_tool_calls);
        }
    }

    std::string response_id;
    if (const auto created_at_it = parsed->find("created_at");
        created_at_it != parsed->end() && created_at_it->is_string()) {
        response_id = created_at_it->get<std::string>();
    }

    return ParsedOllamaToolRoundResponse{
        .output_text = std::move(output_text),
        .response_id = std::move(response_id),
        .assistant_message = std::move(assistant_message),
        .tool_calls = std::move(tool_calls),
        .pending_tool_calls = std::move(pending_tool_calls),
    };
}

class OllamaLlmClient final : public LlmClient {
  public:
    OllamaLlmClient(OllamaLlmClientConfig config, ParsedHttpUrl parsed_base_url)
        : config_(std::move(config)), parsed_base_url_(std::move(parsed_base_url)),
          http_client_(std::make_shared<PersistentHttpClient>(
              parsed_base_url_, HttpClientConfig{
                                    .request_timeout = config_.request_timeout,
                                    .user_agent = config_.user_agent,
                                })) {}

    [[nodiscard]] absl::Status Validate() const override {
        return ValidateOllamaLlmClientConfig(config_);
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return http_client_->WarmUp();
    }

    [[nodiscard]] bool SupportsToolCalling() const override {
        return true;
    }

    [[nodiscard]] absl::Status StreamResponse(const LlmRequest& request,
                                              const LlmEventCallback& on_event) const override {
        if (const absl::Status status = Validate(); !status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("ollama llm request must include model");
        }
        if (request.user_text.empty()) {
            return invalid_argument("ollama llm request must include user_text");
        }

        const absl::StatusOr<bool> think = ResolveThinkFlag(request.reasoning_effort);
        if (!think.ok()) {
            return think.status();
        }

        const absl::StatusOr<std::vector<json>> messages =
            BuildTextMessages(request.system_prompt, request.user_text);
        if (!messages.ok()) {
            return messages.status();
        }

        json body = {
            { "model", request.model },
            { "stream", false },
            { "think", *think },
            { "messages", json::array() },
        };
        for (const json& message : *messages) {
            body["messages"].push_back(message);
        }

        const absl::StatusOr<HttpResponse> response = http_client_->Execute(HttpRequestSpec{
            .method = boost::beast::http::verb::post,
            .target_path = "/api/chat",
            .headers = BuildHeaders(config_),
            .body = body.dump(),
        });
        if (!response.ok()) {
            return response.status();
        }
        if (response->status_code != 200U) {
            return MapHttpFailure(response->status_code, ExtractErrorMessage(*response));
        }

        const absl::StatusOr<ParsedOllamaChatResponse> parsed_response =
            ParseChatResponse(*response);
        if (!parsed_response.ok()) {
            return parsed_response.status();
        }

        if (!parsed_response->output_text.empty()) {
            const absl::Status delta_status = on_event(LlmTextDeltaEvent{
                .text_delta = parsed_response->output_text,
            });
            if (!delta_status.ok()) {
                return delta_status;
            }
        }
        return on_event(LlmCompletedEvent{
            .response_id = parsed_response->response_id,
        });
    }

    [[nodiscard]] absl::StatusOr<LlmToolCallResponse>
    RunToolCallRound(const LlmToolCallRequest& request) const override {
        if (const absl::Status status = Validate(); !status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("ollama tool call request must include model");
        }

        const absl::StatusOr<bool> think = ResolveThinkFlag(request.reasoning_effort);
        if (!think.ok()) {
            return think.status();
        }
        if (request.continuation_token.size() > kMaxToolContinuationBytes) {
            return resource_exhausted("ollama tool continuation_token exceeds maximum length");
        }

        const absl::StatusOr<json> tools_json = ToOllamaToolsJson(request.function_tools);
        if (!tools_json.ok()) {
            return tools_json.status();
        }
        const absl::StatusOr<OllamaToolContinuationState> state =
            DeserializeToolContinuationToken(request.continuation_token);
        if (!state.ok()) {
            return state.status();
        }
        const absl::StatusOr<std::vector<json>> messages =
            BuildToolMessagesForRequest(request.user_text, request.tool_outputs, *state);
        if (!messages.ok()) {
            return messages.status();
        }

        json body = {
            { "model", request.model },    { "stream", false },      { "think", *think },
            { "messages", json::array() }, { "tools", *tools_json },
        };
        if (!request.system_prompt.empty()) {
            body["messages"].push_back(json{
                { "role", "system" },
                { "content", request.system_prompt },
            });
        }
        for (const json& message : *messages) {
            body["messages"].push_back(message);
        }

        const absl::StatusOr<HttpResponse> response = http_client_->Execute(HttpRequestSpec{
            .method = boost::beast::http::verb::post,
            .target_path = "/api/chat",
            .headers = BuildHeaders(config_),
            .body = body.dump(),
        });
        if (!response.ok()) {
            return response.status();
        }
        if (response->status_code != 200U) {
            return MapHttpFailure(response->status_code, ExtractErrorMessage(*response));
        }

        const absl::StatusOr<ParsedOllamaToolRoundResponse> parsed_response =
            ParseToolRoundResponse(*response);
        if (!parsed_response.ok()) {
            return parsed_response.status();
        }

        OllamaToolContinuationState next_state;
        next_state.messages = *messages;
        next_state.messages.push_back(parsed_response->assistant_message);
        next_state.pending_tool_calls = parsed_response->pending_tool_calls;

        const std::string continuation_token = SerializeToolContinuationToken(next_state);
        if (continuation_token.size() > kMaxToolContinuationBytes) {
            return resource_exhausted("ollama tool continuation_token exceeds maximum length");
        }

        return LlmToolCallResponse{
            .output_text = parsed_response->output_text,
            .tool_calls = parsed_response->tool_calls,
            .continuation_token = continuation_token,
            .response_id = parsed_response->response_id,
        };
    }

  private:
    OllamaLlmClientConfig config_;
    ParsedHttpUrl parsed_base_url_;
    std::shared_ptr<PersistentHttpClient> http_client_;
};

} // namespace

absl::Status ValidateOllamaLlmClientConfig(const OllamaLlmClientConfig& config) {
    return ValidateOllamaLlmClientConfigForCreation(config).status();
}

absl::StatusOr<std::shared_ptr<const LlmClient>>
CreateOllamaLlmClient(OllamaLlmClientConfig config) {
    const absl::StatusOr<ParsedHttpUrl> parsed_base_url =
        ValidateOllamaLlmClientConfigForCreation(config);
    if (!parsed_base_url.ok()) {
        return parsed_base_url.status();
    }
    return std::shared_ptr<const LlmClient>(
        std::make_shared<OllamaLlmClient>(std::move(config), *parsed_base_url));
}

} // namespace isla::server
