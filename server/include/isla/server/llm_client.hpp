#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server {

enum class LlmReasoningEffort {
    kNone,
    kMinimal,
    kLow,
    kMedium,
    kHigh,
    kXHigh,
};

struct LlmRequest {
    std::string model;
    std::string system_prompt;
    std::string user_text;
    LlmReasoningEffort reasoning_effort = LlmReasoningEffort::kNone;
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

struct LlmTextDeltaEvent {
    std::string text_delta;
};

struct LlmCompletedEvent {
    std::string response_id;
};

struct LlmFunctionTool {
    std::string name;
    std::string description;
    std::string parameters_json_schema;
    bool strict = true;
};

struct LlmFunctionCall {
    std::string call_id;
    std::string name;
    std::string arguments_json;
};

struct LlmFunctionCallOutput {
    std::string call_id;
    std::string output;
};

struct LlmToolCallRequest {
    std::string model;
    std::string system_prompt;
    std::string user_text;
    std::span<const LlmFunctionTool> function_tools;
    std::span<const LlmFunctionCallOutput> tool_outputs;
    // Opaque provider-owned state returned from the previous round. Callers must
    // treat this as an uninterpreted token.
    std::string continuation_token;
    LlmReasoningEffort reasoning_effort = LlmReasoningEffort::kNone;
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

struct LlmToolCallResponse {
    std::string output_text;
    std::vector<LlmFunctionCall> tool_calls;
    std::string continuation_token;
    std::string response_id;
};

using LlmEvent = std::variant<LlmTextDeltaEvent, LlmCompletedEvent>;
using LlmEventCallback = std::function<absl::Status(const LlmEvent&)>;

class LlmClient {
  public:
    virtual ~LlmClient() = default;

    [[nodiscard]] virtual absl::Status Validate() const = 0;

    // Eagerly establishes any reusable transport state so the first request
    // avoids connection-setup latency when the provider supports warmup.
    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    [[nodiscard]] virtual bool SupportsToolCalling() const {
        return false;
    }

    // Streams response events in provider order. Implementations must invoke
    // `on_event` synchronously and non-concurrently on the calling thread.
    [[nodiscard]] virtual absl::Status StreamResponse(const LlmRequest& request,
                                                      const LlmEventCallback& on_event) const = 0;

    // Executes one round of a tool-calling exchange. Implementations that do
    // not support tool calling may return FailedPreconditionError.
    [[nodiscard]] virtual absl::StatusOr<LlmToolCallResponse>
    RunToolCallRound(const LlmToolCallRequest& request) const {
        static_cast<void>(request);
        return absl::FailedPreconditionError("llm client does not support tool calling");
    }
};

} // namespace isla::server
