#include "isla/server/ai_gateway_session_handler.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"

namespace isla::server::ai_gateway {
namespace protocol = isla::shared::ai_gateway;
namespace {

std::optional<std::string> optional_string(std::optional<std::string_view> value) {
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    return std::string(*value);
}

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

bool IsSupportedTranscriptSeedRole(std::string_view role) {
    return role == "user" || role == "assistant";
}

absl::StatusOr<std::optional<isla::server::memory::Timestamp>>
ParseOptionalTimestamp(std::optional<std::string> value, std::string_view field_name) {
    if (!value.has_value()) {
        return std::nullopt;
    }
    try {
        return isla::server::memory::ParseTimestamp(*value);
    } catch (const std::exception& error) {
        return invalid_argument(std::string(field_name) +
                                " is not a valid timestamp: " + error.what());
    }
}

} // namespace

GatewaySessionHandler::GatewaySessionHandler(std::string session_id,
                                             std::shared_ptr<const TelemetrySink> telemetry_sink)
    : session_id_(std::move(session_id)),
      telemetry_sink_(NormalizeTelemetrySink(std::move(telemetry_sink))) {}

HandleIncomingResult GatewaySessionHandler::HandleIncomingJson(std::string_view json_text) {
    HandleIncomingResult result{};
    const TurnTelemetryContext::Clock::time_point gateway_accept_started =
        TurnTelemetryContext::Clock::now();
    const absl::StatusOr<protocol::GatewayMessage> parsed = protocol::parse_json_message(json_text);
    if (!parsed.ok()) {
        return RejectIncoming(std::nullopt, "bad_request", parsed.status().message());
    }

    const protocol::GatewayMessage& message = *parsed;
    switch (protocol::message_type(message)) {
    case protocol::MessageType::SessionStart: {
        const auto& session_start = std::get<protocol::SessionStartMessage>(message);
        const absl::StatusOr<std::optional<isla::server::memory::Timestamp>> session_start_time =
            ParseOptionalTimestamp(session_start.session_start_time, "session_start_time");
        if (!session_start_time.ok()) {
            return RejectIncoming(std::nullopt, "bad_request",
                                  session_start_time.status().message());
        }
        const absl::StatusOr<std::optional<isla::server::memory::Timestamp>>
            evaluation_reference_time = ParseOptionalTimestamp(
                session_start.evaluation_reference_time, "evaluation_reference_time");
        if (!evaluation_reference_time.ok()) {
            return RejectIncoming(std::nullopt, "bad_request",
                                  evaluation_reference_time.status().message());
        }
        const absl::Status status = session_state_.start(session_id_);
        if (!status.ok()) {
            return RejectIncoming(std::nullopt, "bad_request", status.message());
        }
        result.ok = true;
        result.session_started = SessionStartedEvent{
            .session_id = session_id_,
            .user_id = session_start.user_id,
            .session_start_time = *session_start_time,
            .evaluation_reference_time = *evaluation_reference_time,
        };
        result.outgoing_frames.push_back(encode(protocol::SessionStartedMessage{ session_id_ }));
        return result;
    }
    case protocol::MessageType::SessionEnd: {
        const auto& session_end = std::get<protocol::SessionEndMessage>(message);
        if (session_end.session_id != current_session_id()) {
            return RejectIncoming(std::nullopt, "bad_request",
                                  "session.end session_id does not match active session");
        }

        const absl::Status status = session_state_.end();
        if (!status.ok()) {
            return RejectIncoming(std::nullopt, "bad_request", status.message());
        }

        result.ok = true;
        result.should_close = true;
        result.outgoing_frames.push_back(
            encode(protocol::SessionEndedMessage{ current_session_id() }));
        return result;
    }
    case protocol::MessageType::TranscriptSeed: {
        const auto& transcript_seed = std::get<protocol::TranscriptSeedMessage>(message);
        const absl::StatusOr<std::optional<isla::server::memory::Timestamp>> create_time =
            ParseOptionalTimestamp(transcript_seed.create_time, "create_time");
        if (!create_time.ok()) {
            return RejectIncoming(transcript_seed.turn_id, "bad_request",
                                  create_time.status().message());
        }
        if (session_state_.snapshot().status != protocol::SessionStatus::Active) {
            return RejectIncoming(transcript_seed.turn_id, "bad_request",
                                  "transcript.seed requires an active session");
        }
        if (session_state_.snapshot().active_turn.has_value()) {
            return RejectIncoming(transcript_seed.turn_id, "bad_request",
                                  "transcript.seed is not allowed while a live turn is active");
        }
        if (!IsSupportedTranscriptSeedRole(transcript_seed.role)) {
            return RejectIncoming(transcript_seed.turn_id, "bad_request",
                                  "transcript.seed role must be 'user' or 'assistant'");
        }
        if (transcript_seed.text.size() > kMaxTextInputBytes) {
            return RejectIncoming(transcript_seed.turn_id, "bad_request",
                                  "transcript.seed text exceeds maximum length");
        }

        result.ok = true;
        result.transcript_seed = TranscriptSeedEvent{
            .session_id = current_session_id(),
            .turn_id = transcript_seed.turn_id,
            .role = transcript_seed.role,
            .text = transcript_seed.text,
            .create_time = *create_time,
        };
        return result;
    }
    case protocol::MessageType::TextInput: {
        const auto& text_input = std::get<protocol::TextInputMessage>(message);
        const absl::StatusOr<std::optional<isla::server::memory::Timestamp>> create_time =
            ParseOptionalTimestamp(text_input.create_time, "create_time");
        if (!create_time.ok()) {
            return RejectIncoming(text_input.turn_id, "bad_request",
                                  create_time.status().message());
        }
        if (text_input.text.size() > kMaxTextInputBytes) {
            return RejectIncoming(text_input.turn_id, "bad_request",
                                  "text.input text exceeds maximum length");
        }
        const absl::Status status = session_state_.begin_turn(text_input.turn_id);
        if (!status.ok()) {
            return RejectIncoming(text_input.turn_id, "bad_request", status.message());
        }

        const TurnTelemetryContext::Clock::time_point accepted_at =
            TurnTelemetryContext::Clock::now();
        const std::shared_ptr<const TurnTelemetryContext> telemetry_context =
            MakeTurnTelemetryContext(current_session_id(), text_input.turn_id, telemetry_sink_,
                                     accepted_at);
        RecordTelemetryPhase(telemetry_context, telemetry::kPhaseGatewayAccept,
                             gateway_accept_started, accepted_at);
        RecordTelemetryEvent(telemetry_context, telemetry::kEventTurnAccepted, accepted_at);
        result.ok = true;
        result.accepted_turn = TurnAcceptedEvent{
            .session_id = current_session_id(),
            .turn_id = text_input.turn_id,
            .text = text_input.text,
            .create_time = *create_time,
            .telemetry_context = telemetry_context,
        };
        return result;
    }
    case protocol::MessageType::TurnCancel: {
        const auto& turn_cancel = std::get<protocol::TurnCancelMessage>(message);
        const absl::Status status = session_state_.request_turn_cancel(turn_cancel.turn_id);
        if (!status.ok()) {
            return RejectIncoming(turn_cancel.turn_id, "bad_request", status.message());
        }

        result.ok = true;
        result.cancel_requested = TurnCancelRequestedEvent{ .session_id = current_session_id(),
                                                            .turn_id = turn_cancel.turn_id };
        return result;
    }
    case protocol::MessageType::SessionStarted:
    case protocol::MessageType::SessionEnded:
    case protocol::MessageType::TranscriptSeeded:
    case protocol::MessageType::TextOutput:
    case protocol::MessageType::AudioOutput:
    case protocol::MessageType::TurnCompleted:
    case protocol::MessageType::TurnCancelled:
    case protocol::MessageType::Error:
        return RejectIncoming(std::nullopt, "bad_request",
                              "client sent a server-owned message type");
    }

    return RejectIncoming(std::nullopt, "bad_request", "unsupported incoming message");
}

absl::StatusOr<EmitResult> GatewaySessionHandler::EmitTextOutput(std::string_view turn_id,
                                                                 std::string_view text) {
    if (text.empty()) {
        return invalid_argument("text output must be non-empty");
    }
    if (text.size() > kMaxTextOutputBytes) {
        return absl::ResourceExhaustedError("text output exceeds maximum length");
    }

    const absl::Status status = session_state_.mark_text_output(turn_id);
    if (!status.ok()) {
        return status;
    }

    EmitResult result{};
    result.outgoing_frames.push_back(encode(
        protocol::TextOutputMessage{ .turn_id = std::string(turn_id), .text = std::string(text) }));
    return result;
}

absl::StatusOr<EmitResult> GatewaySessionHandler::EmitAudioOutput(std::string_view turn_id,
                                                                  std::string_view mime_type,
                                                                  std::string_view audio_base64) {
    if (mime_type.empty() || audio_base64.empty()) {
        return invalid_argument("audio output requires non-empty mime_type and audio_base64");
    }

    const absl::Status status = session_state_.mark_audio_output(turn_id);
    if (!status.ok()) {
        return status;
    }

    EmitResult result{};
    result.outgoing_frames.push_back(encode(protocol::AudioOutputMessage{
        .turn_id = std::string(turn_id),
        .mime_type = std::string(mime_type),
        .audio_base64 = std::string(audio_base64),
    }));
    return result;
}

absl::StatusOr<EmitResult>
GatewaySessionHandler::EmitTranscriptSeeded(std::string_view turn_id, std::string_view role) const {
    if (turn_id.empty() || role.empty()) {
        return invalid_argument("transcript seeded emission requires non-empty turn_id and role");
    }
    if (session_state_.snapshot().status != protocol::SessionStatus::Active) {
        return failed_precondition("session must be active to emit transcript seeded");
    }

    EmitResult result{};
    result.outgoing_frames.push_back(encode(protocol::TranscriptSeededMessage{
        .turn_id = std::string(turn_id), .role = std::string(role) }));
    return result;
}

absl::StatusOr<EmitResult> GatewaySessionHandler::EmitTurnCompleted(std::string_view turn_id) {
    const absl::Status status = session_state_.complete_turn(turn_id);
    if (!status.ok()) {
        return status;
    }

    EmitResult result{};
    result.outgoing_frames.push_back(
        encode(protocol::TurnCompletedMessage{ std::string(turn_id) }));
    return result;
}

absl::StatusOr<EmitResult> GatewaySessionHandler::EmitTurnCancelled(std::string_view turn_id) {
    const absl::Status status = session_state_.confirm_turn_cancel(turn_id);
    if (!status.ok()) {
        return status;
    }

    EmitResult result{};
    result.outgoing_frames.push_back(
        encode(protocol::TurnCancelledMessage{ std::string(turn_id) }));
    return result;
}

absl::StatusOr<EmitResult> GatewaySessionHandler::EmitError(std::optional<std::string_view> turn_id,
                                                            std::string_view code,
                                                            std::string_view message) const {
    if (code.empty() || message.empty()) {
        return invalid_argument("error emission requires non-empty code and message");
    }

    EmitResult result{};
    const std::string& session_id_value = current_session_id();
    const std::optional<std::string> session_id =
        session_id_value.empty() ? std::nullopt : std::optional<std::string>(session_id_value);
    result.outgoing_frames.push_back(encode(protocol::ErrorMessage{
        .session_id = session_id,
        .turn_id = optional_string(turn_id),
        .code = std::string(code),
        .message = std::string(message),
    }));
    return result;
}

HandleIncomingResult GatewaySessionHandler::RejectIncoming(std::optional<std::string_view> turn_id,
                                                           std::string_view code,
                                                           std::string_view message) const {
    HandleIncomingResult result{};
    result.error_message = std::string(message);

    absl::StatusOr<EmitResult> emit = EmitError(turn_id, code, message);
    if (emit.ok()) {
        result.outgoing_frames = std::move(emit->outgoing_frames);
    }
    return result;
}

const std::string& GatewaySessionHandler::current_session_id() const {
    return session_state_.snapshot().session_id;
}

std::string GatewaySessionHandler::encode(const protocol::GatewayMessage& message) const {
    return protocol::to_json_string(message);
}

} // namespace isla::server::ai_gateway
