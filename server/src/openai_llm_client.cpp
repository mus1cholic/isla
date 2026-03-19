#include "isla/server/openai_llm_client.hpp"

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "isla/server/openai_reasoning_effort.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server {
namespace {

using isla::server::ai_gateway::OpenAiReasoningEffort;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEvent;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
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

    [[nodiscard]] absl::Status
    StreamResponse(const LlmRequest& request, const LlmEventCallback& on_event) const override {
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
