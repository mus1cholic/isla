#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "absl/status/status.h"

namespace isla::server::ai_gateway {

struct OpenAiResponsesClientConfig {
    bool enabled = false;
    std::string api_key;
    std::string scheme = "https";
    std::string host = "api.openai.com";
    std::uint16_t port = 443;
    std::string target = "/v1/responses";
    std::optional<std::string> organization;
    std::optional<std::string> project;
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(60) };
    std::string user_agent = "isla-ai-gateway/phase-3.5";
};

struct OpenAiResponsesRequest {
    std::string model;
    std::string system_prompt;
    std::string user_text;
};

struct OpenAiResponsesTextDeltaEvent {
    std::string text_delta;
};

struct OpenAiResponsesCompletedEvent {
    std::string response_id;
};

using OpenAiResponsesEvent =
    std::variant<OpenAiResponsesTextDeltaEvent, OpenAiResponsesCompletedEvent>;
using OpenAiResponsesEventCallback = std::function<absl::Status(const OpenAiResponsesEvent&)>;

class OpenAiResponsesClient {
  public:
    virtual ~OpenAiResponsesClient() = default;

    [[nodiscard]] virtual absl::Status Validate() const = 0;
    [[nodiscard]] virtual absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const = 0;
};

[[nodiscard]] std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config);

} // namespace isla::server::ai_gateway
