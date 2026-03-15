#include "isla/server/openai_responses_client.hpp"

#include <optional>
#include <string>
#include <type_traits>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include <nlohmann/json.hpp>

#include "isla/server/openai_responses_curl_transport.hpp"
#include "isla/server/openai_responses_http_utils.hpp"
#include "isla/server/openai_responses_inprocess_transport.hpp"
#include "isla/server/openai_responses_transport_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::StatusOr<TransportStreamResult>
ExecuteTransport(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                 const OpenAiResponsesEventCallback& on_event) {
    if (config.scheme == "http") {
        return ExecuteInProcessHttp(config, request_json, on_event);
    }
#if !defined(_WIN32)
    return ExecuteInProcessHttps(config, request_json, on_event);
#else
    // NOTICE: Native Windows development still uses the curl fallback for HTTPS while the
    // Linux-only server path moves to an in-process TLS client.
    return ExecuteCurl(config, request_json, on_event);
#endif
}

class OpenAiResponsesClientImpl final : public OpenAiResponsesClient {
  public:
    explicit OpenAiResponsesClientImpl(OpenAiResponsesClientConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] absl::Status Validate() const override {
        if (!config_.enabled) {
            return invalid_argument("openai responses client is disabled");
        }
        if (config_.api_key.empty()) {
            return invalid_argument("openai responses api_key must not be empty");
        }
        if (absl::Status status =
                ValidateHttpFieldValue("openai responses api_key", config_.api_key);
            !status.ok()) {
            return status;
        }
        if (config_.host.empty()) {
            return invalid_argument("openai responses host must not be empty");
        }
        if (absl::Status status = ValidateHttpHostValue(config_.host); !status.ok()) {
            return status;
        }
        if (config_.target.empty() || config_.target.front() != '/') {
            return invalid_argument("openai responses target must start with '/'");
        }
        if (absl::Status status = ValidateHttpTargetValue(config_.target); !status.ok()) {
            return status;
        }
        if (config_.scheme != "http" && config_.scheme != "https") {
            return invalid_argument("openai responses scheme must be 'http' or 'https'");
        }
        if (config_.request_timeout <= std::chrono::milliseconds::zero()) {
            return invalid_argument("openai responses request_timeout must be positive");
        }
        if (absl::Status status =
                ValidateHttpFieldValue("openai responses user_agent", config_.user_agent);
            !status.ok()) {
            return status;
        }
        if (config_.organization.has_value()) {
            if (absl::Status status =
                    ValidateHttpFieldValue("openai responses organization", *config_.organization);
                !status.ok()) {
                return status;
            }
        }
        if (config_.project.has_value()) {
            if (absl::Status status =
                    ValidateHttpFieldValue("openai responses project", *config_.project);
                !status.ok()) {
                return status;
            }
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        absl::Status status = Validate();
        if (!status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("openai responses request must include model");
        }
        if (request.user_text.empty()) {
            return invalid_argument("openai responses request must include user_text");
        }
        const std::optional<std::string_view> reasoning_effort =
            TryOpenAiReasoningEffortToString(request.reasoning_effort);
        if (!reasoning_effort.has_value()) {
            return invalid_argument("openai responses request reasoning_effort is invalid");
        }

        const TurnTelemetryContext::Clock::time_point serialize_started_at =
            TurnTelemetryContext::Clock::now();
        nlohmann::json body = {
            { "model", request.model },
            { "input", request.user_text },
            { "reasoning",
              {
                  { "effort", *reasoning_effort },
              } },
            { "stream", true },
        };
        if (!request.system_prompt.empty()) {
            body["instructions"] = request.system_prompt;
        }

        VLOG(1) << "AI gateway openai responses dispatching host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target) << "' model='"
                << SanitizeForLog(request.model) << "' reasoning_effort='" << *reasoning_effort
                << "' timeout_ms=" << config_.request_timeout.count()
                << " user_text_bytes=" << request.user_text.size()
                << " system_prompt_present=" << (!request.system_prompt.empty() ? "true" : "false");

        const std::string request_json = body.dump();
        const TurnTelemetryContext::Clock::time_point serialize_completed_at =
            TurnTelemetryContext::Clock::now();
        RecordTelemetryPhase(request.telemetry_context, telemetry::kPhaseProviderSerializeRequest,
                             serialize_started_at, serialize_completed_at);

        std::optional<ScopedTelemetryPhase> provider_stream_phase;
        bool recorded_first_token = false;
        const OpenAiResponsesEventCallback telemetry_on_event =
            [&request, &on_event, &provider_stream_phase,
             &recorded_first_token](const OpenAiResponsesEvent& event) -> absl::Status {
            const TurnTelemetryContext::Clock::time_point event_at =
                TurnTelemetryContext::Clock::now();
            if (!provider_stream_phase.has_value()) {
                provider_stream_phase.emplace(request.telemetry_context,
                                              telemetry::kPhaseProviderStream, "", "", event_at);
            }
            return std::visit(
                [&request, &on_event, &provider_stream_phase, &recorded_first_token,
                 event_at](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        if (!recorded_first_token && !concrete_event.text_delta.empty()) {
                            recorded_first_token = true;
                            RecordTelemetryEvent(request.telemetry_context,
                                                 telemetry::kEventProviderFirstToken, event_at);
                        }
                    } else if constexpr (std::is_same_v<Event, OpenAiResponsesCompletedEvent>) {
                        RecordTelemetryEvent(request.telemetry_context,
                                             telemetry::kEventProviderCompleted, event_at);
                    }

                    const absl::Status callback_status =
                        on_event(OpenAiResponsesEvent(concrete_event));
                    if (provider_stream_phase.has_value() &&
                        std::is_same_v<Event, OpenAiResponsesCompletedEvent>) {
                        provider_stream_phase->Finish(event_at);
                    }
                    return callback_status;
                },
                event);
        };

        const TurnTelemetryContext::Clock::time_point transport_started_at =
            TurnTelemetryContext::Clock::now();
        RecordTelemetryEvent(request.telemetry_context, telemetry::kEventProviderDispatched,
                             transport_started_at);
        ScopedTelemetryPhase transport_phase(request.telemetry_context,
                                             telemetry::kPhaseProviderTransport, "", "",
                                             transport_started_at);
        const absl::StatusOr<TransportStreamResult> stream_result =
            ExecuteTransport(config_, request_json, telemetry_on_event);
        transport_phase.Finish();
        if (!stream_result.ok()) {
            LOG(ERROR) << "AI gateway openai responses request failed host='"
                       << SanitizeForLog(config_.host) << "' target='"
                       << SanitizeForLog(config_.target) << "' detail='"
                       << SanitizeForLog(stream_result.status().message()) << "'";
            return stream_result.status();
        }
        VLOG(1) << "AI gateway openai responses completed host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target)
                << "' body_bytes=" << stream_result->body_bytes
                << " saw_delta=" << (stream_result->parse_summary.saw_delta ? "true" : "false")
                << " saw_completed="
                << (stream_result->parse_summary.saw_completed ? "true" : "false")
                << " event_count=" << stream_result->parse_summary.event_count;
        return absl::OkStatus();
    }

  private:
    OpenAiResponsesClientConfig config_;
};

} // namespace

std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config) {
    return std::make_shared<OpenAiResponsesClientImpl>(std::move(config));
}

} // namespace isla::server::ai_gateway
