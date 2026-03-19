#pragma once

#include <functional>
#include <memory>
#include <string>
#include <variant>

#include "absl/status/status.h"
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

    // Streams response events in provider order. Implementations must invoke
    // `on_event` synchronously and non-concurrently on the calling thread.
    [[nodiscard]] virtual absl::Status StreamResponse(const LlmRequest& request,
                                                      const LlmEventCallback& on_event) const = 0;
};

} // namespace isla::server
