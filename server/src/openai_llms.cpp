#include "isla/server/openai_llms.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "isla/server/llm_client.hpp"

namespace isla::server::ai_gateway {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

absl::Status resource_exhausted(std::string_view message) {
    return absl::ResourceExhaustedError(std::string(message));
}

absl::Status invalid_reasoning_effort() {
    return absl::InvalidArgumentError("openai llms reasoning_effort is invalid");
}

absl::StatusOr<isla::server::LlmReasoningEffort>
ToLlmReasoningEffort(OpenAiReasoningEffort effort) {
    switch (effort) {
    case OpenAiReasoningEffort::kNone:
        return isla::server::LlmReasoningEffort::kNone;
    case OpenAiReasoningEffort::kMinimal:
        return isla::server::LlmReasoningEffort::kMinimal;
    case OpenAiReasoningEffort::kLow:
        return isla::server::LlmReasoningEffort::kLow;
    case OpenAiReasoningEffort::kMedium:
        return isla::server::LlmReasoningEffort::kMedium;
    case OpenAiReasoningEffort::kHigh:
        return isla::server::LlmReasoningEffort::kHigh;
    case OpenAiReasoningEffort::kXHigh:
        return isla::server::LlmReasoningEffort::kXHigh;
    }
    return invalid_reasoning_effort();
}

} // namespace

OpenAiLLMs::OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model,
                       std::shared_ptr<const isla::server::LlmClient> llm_client,
                       OpenAiReasoningEffort reasoning_effort)
    : step_name_(std::move(step_name)), system_prompt_(std::move(system_prompt)),
      model_(std::move(model)), reasoning_effort_(reasoning_effort),
      llm_client_(std::move(llm_client)) {}

const std::string& OpenAiLLMs::step_name() const {
    return step_name_;
}

absl::Status OpenAiLLMs::Validate() const {
    if (step_name_.empty()) {
        return invalid_argument("openai llms must include a step_name");
    }
    if (model_.empty()) {
        return invalid_argument("openai llms must include a model");
    }
    if (!TryOpenAiReasoningEffortToString(reasoning_effort_).has_value()) {
        return invalid_reasoning_effort();
    }
    return absl::OkStatus();
}

absl::Status OpenAiLLMs::ValidateInput(const ExecutionRuntimeInput& runtime_input) const {
    if (runtime_input.user_text.empty()) {
        return invalid_argument("openai llms input must include user_text");
    }
    return absl::OkStatus();
}

absl::StatusOr<ExecutionStepResult>
OpenAiLLMs::GenerateContent(std::size_t item_index,
                            const ExecutionRuntimeInput& runtime_input) const {
    static_cast<void>(item_index);

    absl::Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    absl::Status input_status = ValidateInput(runtime_input);
    if (!input_status.ok()) {
        return input_status;
    }

    std::string output_text;
    try {
        const absl::StatusOr<std::string> provider_text =
            GenerateProviderResponse(item_index, runtime_input);
        if (!provider_text.ok()) {
            return provider_text.status();
        }
        output_text = *provider_text;
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='" << SanitizeForLog(error.what())
                   << "'";
        return absl::InternalError("openai llms response building failed");
    } catch (...) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='unknown exception'";
        return absl::InternalError("openai llms response building failed");
    }

    return ExecutionStepResult(LlmCallResult{
        .step_name = step_name_,
        .model = model_,
        .output_text = std::move(output_text),
    });
}

absl::StatusOr<std::string>
OpenAiLLMs::GenerateProviderResponse(std::size_t item_index,
                                     const ExecutionRuntimeInput& runtime_input) const {
    static_cast<void>(item_index);
    ScopedTelemetryPhase provider_total_phase(runtime_input.telemetry_context,
                                              telemetry::kPhaseLlmProviderTotal);

    if (llm_client_ == nullptr) {
        LOG(ERROR) << "AI gateway openai llms missing llm client step_name='"
                   << SanitizeForLog(step_name_) << "' model='" << SanitizeForLog(model_) << "'";
        return failed_precondition("openai llms requires a configured llm client");
    }

    absl::Status client_status = llm_client_->Validate();
    if (!client_status.ok()) {
        return client_status;
    }

    const std::string_view effective_system_prompt =
        runtime_input.system_prompt.empty() ? std::string_view(system_prompt_)
                                            : std::string_view(runtime_input.system_prompt);
    const std::optional<std::string_view> reasoning_effort =
        TryOpenAiReasoningEffortToString(reasoning_effort_);
    if (!reasoning_effort.has_value()) {
        return invalid_reasoning_effort();
    }
    const absl::StatusOr<isla::server::LlmReasoningEffort> llm_reasoning_effort =
        ToLlmReasoningEffort(reasoning_effort_);
    if (!llm_reasoning_effort.ok()) {
        return llm_reasoning_effort.status();
    }

    VLOG(1) << "AI gateway openai llms dispatching provider request step_name='"
            << SanitizeForLog(step_name_) << "' model='" << SanitizeForLog(model_)
            << "' reasoning_effort='" << *reasoning_effort
            << "' user_text_bytes=" << runtime_input.user_text.size()
            << " system_prompt_present=" << (!effective_system_prompt.empty() ? "true" : "false");

    std::string output_text;
    std::optional<TurnTelemetryContext::Clock::time_point> first_aggregate_started_at;
    std::optional<TurnTelemetryContext::Clock::time_point> last_aggregate_completed_at;
    absl::Status stream_status = llm_client_->StreamResponse(
        isla::server::LlmRequest{
            .model = model_,
            .system_prompt = std::string(effective_system_prompt),
            .user_text = runtime_input.user_text,
            .reasoning_effort = *llm_reasoning_effort,
            .telemetry_context = runtime_input.telemetry_context,
        },
        [&output_text, &first_aggregate_started_at,
         &last_aggregate_completed_at](const isla::server::LlmEvent& event) -> absl::Status {
            return std::visit(
                [&output_text, &first_aggregate_started_at,
                 &last_aggregate_completed_at](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, isla::server::LlmTextDeltaEvent>) {
                        if (concrete_event.text_delta.empty()) {
                            return absl::OkStatus();
                        }
                        if (output_text.size() + concrete_event.text_delta.size() >
                            kMaxTextOutputBytes) {
                            return resource_exhausted("openai llms output exceeds maximum length");
                        }
                        const TurnTelemetryContext::Clock::time_point aggregate_started_at =
                            TurnTelemetryContext::Clock::now();
                        output_text.append(concrete_event.text_delta);
                        const TurnTelemetryContext::Clock::time_point aggregate_completed_at =
                            TurnTelemetryContext::Clock::now();
                        if (!first_aggregate_started_at.has_value()) {
                            first_aggregate_started_at = aggregate_started_at;
                        }
                        last_aggregate_completed_at = aggregate_completed_at;
                    }
                    return absl::OkStatus();
                },
                event);
        });
    if (first_aggregate_started_at.has_value() && last_aggregate_completed_at.has_value()) {
        RecordTelemetryPhase(runtime_input.telemetry_context,
                             telemetry::kPhaseProviderAggregateText, *first_aggregate_started_at,
                             *last_aggregate_completed_at);
    }
    if (!stream_status.ok()) {
        LOG(ERROR) << "AI gateway openai llms provider request failed step_name='"
                   << SanitizeForLog(step_name_) << "' model='" << SanitizeForLog(model_)
                   << "' detail='" << SanitizeForLog(stream_status.message()) << "'";
        return stream_status;
    }
    return output_text;
}

} // namespace isla::server::ai_gateway
