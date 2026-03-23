#include "isla/server/ollama_llm_client.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_json_client.hpp"

namespace isla::server {
namespace {

using nlohmann::json;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
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

struct ParsedOllamaChatResponse {
    std::string output_text;
    std::string response_id;
};

absl::StatusOr<ParsedOllamaChatResponse> ParseChatResponse(const HttpResponse& response) {
    const json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded()) {
        return failed_precondition("ollama chat response must be valid JSON");
    }
    if (!parsed.is_object()) {
        return failed_precondition("ollama chat response must be a JSON object");
    }

    const auto message_it = parsed.find("message");
    if (message_it == parsed.end() || !message_it->is_object()) {
        return failed_precondition("ollama chat response must include a message object");
    }
    const json& message = *message_it;

    if (const auto tool_calls_it = message.find("tool_calls");
        tool_calls_it != message.end() && tool_calls_it->is_array() && !tool_calls_it->empty()) {
        return failed_precondition("ollama llm client does not support tool calls");
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
    if (const auto created_at_it = parsed.find("created_at");
        created_at_it != parsed.end() && created_at_it->is_string()) {
        response_id = created_at_it->get<std::string>();
    }

    return ParsedOllamaChatResponse{
        .output_text = std::move(content),
        .response_id = std::move(response_id),
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

        json body = {
            { "model", request.model },
            { "stream", false },
            { "think", *think },
            { "messages", json::array() },
        };
        if (!request.system_prompt.empty()) {
            body["messages"].push_back({
                { "role", "system" },
                { "content", request.system_prompt },
            });
        }
        body["messages"].push_back({
            { "role", "user" },
            { "content", request.user_text },
        });

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
