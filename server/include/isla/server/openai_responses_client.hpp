#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/openai_reasoning_effort.hpp"

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
    std::optional<std::string> trusted_ca_cert_pem;
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(60) };
    std::string user_agent = "isla-ai-gateway/phase-3.5";
};

struct OpenAiResponsesRequest {
    std::string model;
    std::string system_prompt;
    std::string user_text;
    OpenAiReasoningEffort reasoning_effort = OpenAiReasoningEffort::kNone;
    std::shared_ptr<const TurnTelemetryContext> telemetry_context;
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

    // Eagerly establishes the underlying transport connection (TCP/TLS) so that
    // the first StreamResponse() call does not pay the connection-setup latency.
    // Default implementation is a no-op for clients that do not maintain
    // persistent connections. Safe to call multiple times.
    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    // Streams response events in upstream order. Implementations must invoke `on_event`
    // synchronously and non-concurrently on the calling thread. Returning a non-OK status from
    // `on_event` must abort streaming and be propagated from `StreamResponse(...)`. A successful
    // return from `StreamResponse(...)` guarantees that the stream reached its terminal completed
    // event.
    [[nodiscard]] virtual absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const = 0;
};

[[nodiscard]] std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config);

} // namespace isla::server::ai_gateway
