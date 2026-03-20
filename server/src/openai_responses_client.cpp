#include "isla/server/openai_responses_client.hpp"

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

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

absl::StatusOr<nlohmann::json> SerializeInputItem(const OpenAiResponsesInputItem& item) {
    return std::visit(
        [](const auto& concrete_item) -> absl::StatusOr<nlohmann::json> {
            using Item = std::decay_t<decltype(concrete_item)>;
            if constexpr (std::is_same_v<Item, OpenAiResponsesRawInputItem>) {
                const nlohmann::json parsed =
                    nlohmann::json::parse(concrete_item.raw_json, nullptr, false);
                if (parsed.is_discarded()) {
                    return invalid_argument(
                        "openai responses raw input item must contain valid JSON");
                }
                return parsed;
            } else if constexpr (std::is_same_v<Item, OpenAiResponsesMessageInputItem>) {
                return nlohmann::json{
                    { "role", concrete_item.role },
                    { "content", concrete_item.content },
                };
            } else {
                return nlohmann::json{
                    { "type", "function_call_output" },
                    { "call_id", concrete_item.call_id },
                    { "output", concrete_item.output },
                };
            }
        },
        item);
}

absl::StatusOr<nlohmann::json>
SerializeFunctionTools(const std::vector<OpenAiResponsesFunctionTool>& function_tools) {
    std::vector<nlohmann::json> tools;
    tools.reserve(function_tools.size());
    for (const OpenAiResponsesFunctionTool& tool : function_tools) {
        if (tool.name.empty()) {
            return invalid_argument("openai responses function tool name must not be empty");
        }
        if (tool.parameters_json_schema.empty()) {
            return invalid_argument(
                "openai responses function tool parameters_json_schema must not be empty");
        }
        const nlohmann::json parameters =
            nlohmann::json::parse(tool.parameters_json_schema, nullptr, false);
        if (!parameters.is_object()) {
            return invalid_argument(
                "openai responses function tool parameters_json_schema must be a JSON object");
        }
        tools.push_back(nlohmann::json{
            { "type", "function" },
            { "name", tool.name },
            { "description", tool.description },
            { "parameters", parameters },
            { "strict", tool.strict },
        });
    }
    return nlohmann::json(std::move(tools));
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
        : config_(std::move(config)) {
        const bool in_process_available = config_.scheme == "http"
#if !defined(_WIN32)
                                          || config_.scheme == "https"
#endif
            ;
        if (in_process_available) {
            transport_ = std::make_unique<PersistentInProcessTransport>(config_);
        }
    }

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

    [[nodiscard]] absl::Status WarmUp() const override {
        if (transport_ != nullptr) {
            return transport_->WarmUp();
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
        if (request.user_text.empty() && request.input_items.empty()) {
            return invalid_argument(
                "openai responses request must include user_text or input_items");
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
            { "reasoning",
              {
                  { "effort", *reasoning_effort },
              } },
            { "stream", true },
        };
        if (request.input_items.empty()) {
            body["input"] = request.user_text;
        } else {
            std::vector<nlohmann::json> input_items;
            input_items.reserve(request.input_items.size() + (request.user_text.empty() ? 0U : 1U));
            if (!request.user_text.empty()) {
                input_items.push_back(nlohmann::json{
                    { "role", "user" },
                    { "content", request.user_text },
                });
            }
            for (const OpenAiResponsesInputItem& item : request.input_items) {
                const absl::StatusOr<nlohmann::json> serialized_item = SerializeInputItem(item);
                if (!serialized_item.ok()) {
                    return serialized_item.status();
                }
                input_items.push_back(*serialized_item);
            }
            body["input"] = nlohmann::json(std::move(input_items));
        }
        if (!request.system_prompt.empty()) {
            body["instructions"] = request.system_prompt;
        }
        if (!request.function_tools.empty()) {
            const absl::StatusOr<nlohmann::json> tools =
                SerializeFunctionTools(request.function_tools);
            if (!tools.ok()) {
                return tools.status();
            }
            body["tools"] = *tools;
            body["parallel_tool_calls"] = request.parallel_tool_calls;
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

        const TurnTelemetryContext::Clock::time_point transport_started_at =
            TurnTelemetryContext::Clock::now();
        std::optional<ScopedTelemetryPhase> provider_stream_phase;
        bool recorded_first_sse_event = false;
        bool recorded_first_token = false;
        const OpenAiResponsesEventCallback telemetry_on_event =
            [&request, &on_event, &provider_stream_phase, &recorded_first_sse_event,
             &recorded_first_token,
             &transport_started_at](const OpenAiResponsesEvent& event) -> absl::Status {
            const TurnTelemetryContext::Clock::time_point event_at =
                TurnTelemetryContext::Clock::now();
            if (!provider_stream_phase.has_value()) {
                provider_stream_phase.emplace(request.telemetry_context,
                                              telemetry::kPhaseProviderStream, "", "", event_at);
            }
            if (!recorded_first_sse_event) {
                recorded_first_sse_event = true;
                RecordTelemetryPhase(request.telemetry_context,
                                     telemetry::kPhaseProviderFirstSseEvent, transport_started_at,
                                     event_at);
            }
            return std::visit(
                [&request, &on_event, &provider_stream_phase, &recorded_first_token,
                 &transport_started_at, event_at](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        if (!recorded_first_token && !concrete_event.text_delta.empty()) {
                            recorded_first_token = true;
                            RecordTelemetryPhase(request.telemetry_context,
                                                 telemetry::kPhaseProviderFirstTextDelta,
                                                 transport_started_at, event_at);
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

        RecordTelemetryEvent(request.telemetry_context, telemetry::kEventProviderDispatched,
                             transport_started_at);
        ScopedTelemetryPhase transport_phase(request.telemetry_context,
                                             telemetry::kPhaseProviderTransport, "", "",
                                             transport_started_at);
        const absl::StatusOr<TransportStreamResult> stream_result =
            transport_ != nullptr ? transport_->Execute(request_json, telemetry_on_event)
                                  : ExecuteTransport(config_, request_json, telemetry_on_event);
        transport_phase.Finish();
        if (!stream_result.ok()) {
            LOG(ERROR) << "AI gateway openai responses request failed host='"
                       << SanitizeForLog(config_.host) << "' target='"
                       << SanitizeForLog(config_.target) << "' detail='"
                       << SanitizeForLog(stream_result.status().message()) << "'";
            return stream_result.status();
        }
        if (stream_result->response_headers_at.has_value()) {
            RecordTelemetryPhase(request.telemetry_context,
                                 telemetry::kPhaseProviderResponseHeaders, transport_started_at,
                                 *stream_result->response_headers_at);
        }
        if (stream_result->first_body_byte_at.has_value()) {
            RecordTelemetryPhase(request.telemetry_context, telemetry::kPhaseProviderFirstBodyByte,
                                 transport_started_at, *stream_result->first_body_byte_at);
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
    mutable std::unique_ptr<PersistentInProcessTransport> transport_;
};

} // namespace

std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config) {
    return std::make_shared<OpenAiResponsesClientImpl>(std::move(config));
}

} // namespace isla::server::ai_gateway
