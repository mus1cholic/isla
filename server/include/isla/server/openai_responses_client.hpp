#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/openai_reasoning_effort.hpp"

namespace isla::server::ai_gateway {

// Provider-facing function tool definition for the Responses API.
//
// This is intentionally separate from the provider-neutral `tools::ToolDefinition` so the gateway
// can evolve its internal tool contracts without leaking provider-specific fields everywhere.
struct OpenAiResponsesFunctionTool {
    std::string name;
    std::string description;
    std::string parameters_json_schema;
    bool strict = true;
};

// Input item passed to the Responses API when continuing a tool-calling loop.
//
// `raw_json` is used to replay provider-owned items from a previous response verbatim, including
// reasoning and function_call items that reasoning models expect to see again alongside tool
// outputs. It should contain one complete JSON object.
struct OpenAiResponsesRawInputItem {
    std::string raw_json;
};

// Explicit user/assistant message item for the Responses API input list.
struct OpenAiResponsesMessageInputItem {
    std::string role;
    std::string content;
};

// Function output item sent back to the model after the application executes a tool call.
struct OpenAiResponsesFunctionCallOutputInputItem {
    std::string call_id;
    std::string output;
};

using OpenAiResponsesInputItem =
    std::variant<OpenAiResponsesRawInputItem, OpenAiResponsesMessageInputItem,
                 OpenAiResponsesFunctionCallOutputInputItem>;

// Output item surfaced by the Responses API in `response.output`.
//
// `raw_json` preserves a semantically equivalent provider object suitable for later replay.
// It is not guaranteed to match the original provider bytes exactly after parse/dump
// normalization. For function calls, `call_id`, `name`, and `arguments_json` expose the fields
// the gateway needs for application tool dispatch.
struct OpenAiResponsesOutputItem {
    std::string type;
    std::string raw_json;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
    std::optional<std::string> arguments_json;
};

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

// Request payload for one OpenAI Responses API call.
//
// `input_items` and `function_tools` are non-owning views so callers can reuse immutable vectors
// across tool-loop rounds without copying them into each request object. Their backing storage must
// stay alive for the duration of `StreamResponse(...)`.
struct OpenAiResponsesRequest {
    std::string model;
    std::string system_prompt;
    std::string user_text;
    std::span<const OpenAiResponsesInputItem> input_items;
    std::span<const OpenAiResponsesFunctionTool> function_tools;
    bool parallel_tool_calls = true;
    OpenAiReasoningEffort reasoning_effort = OpenAiReasoningEffort::kNone;
    std::shared_ptr<const TurnTelemetryContext> telemetry_context;
};

struct OpenAiResponsesTextDeltaEvent {
    std::string text_delta;
};

struct OpenAiResponsesCompletedEvent {
    std::string response_id;
    std::vector<OpenAiResponsesOutputItem> output_items;
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
