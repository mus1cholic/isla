#include "ai_gateway_session_handler.hpp"

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

} // namespace

GatewaySessionHandler::GatewaySessionHandler(std::string session_id)
    : session_id_(std::move(session_id)) {}

HandleIncomingResult GatewaySessionHandler::HandleIncomingJson(std::string_view json_text) {
    HandleIncomingResult result{};
    const absl::StatusOr<protocol::GatewayMessage> parsed = protocol::parse_json_message(json_text);
    if (!parsed.ok()) {
        return RejectIncoming(std::nullopt, "bad_request", parsed.status().message());
    }

    const protocol::GatewayMessage& message = *parsed;
    switch (protocol::message_type(message)) {
    case protocol::MessageType::SessionStart: {
        const absl::Status status = session_state_.start(session_id_);
        if (!status.ok()) {
            return RejectIncoming(std::nullopt, "bad_request", status.message());
        }
        result.ok = true;
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
    case protocol::MessageType::TextInput: {
        const auto& text_input = std::get<protocol::TextInputMessage>(message);
        const absl::Status status = session_state_.begin_turn(text_input.turn_id);
        if (!status.ok()) {
            return RejectIncoming(text_input.turn_id, "bad_request", status.message());
        }

        result.ok = true;
        result.accepted_turn = TurnAcceptedEvent{
            .session_id = current_session_id(),
            .turn_id = text_input.turn_id,
            .text = text_input.text,
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
