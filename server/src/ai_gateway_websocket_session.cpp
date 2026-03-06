#include "ai_gateway_websocket_session.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

const char* close_reason_name(SessionCloseReason reason) {
    switch (reason) {
    case SessionCloseReason::ProtocolEnded:
        return "protocol_ended";
    case SessionCloseReason::TransportClosed:
        return "transport_closed";
    case SessionCloseReason::TransportError:
        return "transport_error";
    case SessionCloseReason::SendFailed:
        return "send_failed";
    case SessionCloseReason::ServerStopping:
        return "server_stopping";
    }
    return "unknown";
}

} // namespace

SequentialSessionIdGenerator::SequentialSessionIdGenerator(std::string prefix)
    : prefix_(std::move(prefix)) {}

std::string SequentialSessionIdGenerator::NextSessionId() {
    return prefix_ + std::to_string(next_id_.fetch_add(1));
}

GatewayWebSocketSessionAdapter::GatewayWebSocketSessionAdapter(
    std::string session_id, GatewayWebSocketConnection& connection,
    GatewaySessionEventSink* event_sink)
    : session_id_(std::move(session_id)), connection_(connection), event_sink_(event_sink),
      handler_(session_id_) {}

absl::Status GatewayWebSocketSessionAdapter::HandleIncomingTextFrame(std::string_view frame) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const HandleIncomingResult result = handler_.HandleIncomingJson(frame);
    const absl::Status send_status = SendFrames(result.outgoing_frames);
    if (!send_status.ok()) {
        return send_status;
    }

    if (result.accepted_turn.has_value() && event_sink_ != nullptr) {
        event_sink_->OnTurnAccepted(*result.accepted_turn);
    }
    if (result.cancel_requested.has_value() && event_sink_ != nullptr) {
        event_sink_->OnTurnCancelRequested(*result.cancel_requested);
    }

    if (result.accepted_turn.has_value()) {
        VLOG(1) << "AI gateway session=" << session_id_
                << " accepted text turn turn_id=" << SanitizeForLog(result.accepted_turn->turn_id);
    }
    if (result.cancel_requested.has_value()) {
        VLOG(1) << "AI gateway session=" << session_id_ << " accepted turn cancellation turn_id="
                << SanitizeForLog(result.cancel_requested->turn_id);
    }
    if (result.should_close) {
        VLOG(1) << "AI gateway session=" << session_id_ << " ended by protocol";
        CloseSession(SessionCloseReason::ProtocolEnded, "session ended", std::nullopt, true);
    }

    if (!result.ok) {
        const auto snapshot = handler_.snapshot();
        const char* log_level = snapshot.status == isla::shared::ai_gateway::SessionStatus::Active
                                    ? "active"
                                    : "inactive";
        VLOG(1) << "AI gateway session=" << session_id_ << " rejected inbound frame while "
                << log_level << " reason='" << SanitizeForLog(result.error_message) << "'";
        return failed_precondition(result.error_message);
    }

    if (handler_.snapshot().status == isla::shared::ai_gateway::SessionStatus::Active &&
        !handler_.snapshot().active_turn.has_value() && result.outgoing_frames.size() == 1U) {
        VLOG(1) << "AI gateway session=" << session_id_ << " started";
    }
    return absl::OkStatus();
}

absl::Status GatewayWebSocketSessionAdapter::EmitTextOutput(std::string_view turn_id,
                                                            std::string_view text) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const absl::StatusOr<EmitResult> emit_result = handler_.EmitTextOutput(turn_id, text);
    if (!emit_result.ok()) {
        return emit_result.status();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " emitting text output turn_id=" << SanitizeForLog(turn_id);
    return SendFrames(emit_result->outgoing_frames);
}

absl::Status GatewayWebSocketSessionAdapter::EmitAudioOutput(std::string_view turn_id,
                                                             std::string_view mime_type,
                                                             std::string_view audio_base64) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const absl::StatusOr<EmitResult> emit_result =
        handler_.EmitAudioOutput(turn_id, mime_type, audio_base64);
    if (!emit_result.ok()) {
        return emit_result.status();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " emitting audio output turn_id=" << SanitizeForLog(turn_id)
            << " mime_type=" << SanitizeForLog(mime_type);
    return SendFrames(emit_result->outgoing_frames);
}

absl::Status GatewayWebSocketSessionAdapter::EmitTurnCompleted(std::string_view turn_id) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const absl::StatusOr<EmitResult> emit_result = handler_.EmitTurnCompleted(turn_id);
    if (!emit_result.ok()) {
        return emit_result.status();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " emitting turn completed turn_id=" << SanitizeForLog(turn_id);
    return SendFrames(emit_result->outgoing_frames);
}

absl::Status GatewayWebSocketSessionAdapter::EmitTurnCancelled(std::string_view turn_id) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const absl::StatusOr<EmitResult> emit_result = handler_.EmitTurnCancelled(turn_id);
    if (!emit_result.ok()) {
        return emit_result.status();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " emitting turn cancelled turn_id=" << SanitizeForLog(turn_id);
    return SendFrames(emit_result->outgoing_frames);
}

absl::Status GatewayWebSocketSessionAdapter::EmitError(std::optional<std::string_view> turn_id,
                                                       std::string_view code,
                                                       std::string_view message) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const absl::StatusOr<EmitResult> emit_result = handler_.EmitError(turn_id, code, message);
    if (!emit_result.ok()) {
        return emit_result.status();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " emitting error code=" << SanitizeForLog(code) << " turn_id='"
            << (turn_id.has_value() ? SanitizeForLog(*turn_id) : std::string("<none>")) << "'";
    return SendFrames(emit_result->outgoing_frames);
}

absl::Status GatewayWebSocketSessionAdapter::HandleTransportError(std::string_view message) {
    if (closed_) {
        return failed_precondition("websocket session is closed");
    }

    const bool session_started =
        handler_.snapshot().status == isla::shared::ai_gateway::SessionStatus::Active;
    const std::optional<std::string> inflight_turn_id = active_turn_id();
    LOG(WARNING) << "AI gateway session=" << session_id_ << " transport error detail='"
                 << SanitizeForLog(message)
                 << "' session_started=" << (session_started ? "true" : "false")
                 << " inflight_turn_id='"
                 << (inflight_turn_id.has_value() ? SanitizeForLog(*inflight_turn_id)
                                                  : std::string("<none>"))
                 << "'";

    if (session_started && inflight_turn_id.has_value()) {
        const auto emit_and_send =
            [this](const absl::StatusOr<EmitResult>& emit_result) -> absl::Status {
            if (!emit_result.ok()) {
                return absl::OkStatus();
            }
            return SendFrames(emit_result->outgoing_frames);
        };

        const absl::Status error_status =
            emit_and_send(handler_.EmitError(*inflight_turn_id, "transport_error", message));
        if (!error_status.ok()) {
            return error_status;
        }

        const absl::Status completed_status =
            emit_and_send(handler_.EmitTurnCompleted(*inflight_turn_id));
        if (!completed_status.ok()) {
            return completed_status;
        }
    }

    CloseSession(SessionCloseReason::TransportError, message, inflight_turn_id, true);
    return absl::OkStatus();
}

void GatewayWebSocketSessionAdapter::HandleSendFailure(std::string_view message) {
    if (closed_) {
        return;
    }

    LOG(WARNING) << "AI gateway session=" << session_id_ << " send failure detail='"
                 << SanitizeForLog(message) << "'";
    CloseSession(SessionCloseReason::SendFailed, message, active_turn_id(), false);
}

void GatewayWebSocketSessionAdapter::HandleTransportClosed() {
    if (closed_) {
        return;
    }

    const std::optional<std::string> inflight_turn_id = active_turn_id();
    LOG(WARNING) << "AI gateway session=" << session_id_ << " transport closed"
                 << " session_started="
                 << (handler_.snapshot().status !=
                             isla::shared::ai_gateway::SessionStatus::NotStarted
                         ? "true"
                         : "false")
                 << " inflight_turn_id='"
                 << (inflight_turn_id.has_value() ? SanitizeForLog(*inflight_turn_id)
                                                  : std::string("<none>"))
                 << "'";
    CloseSession(SessionCloseReason::TransportClosed, "", inflight_turn_id, false);
}

void GatewayWebSocketSessionAdapter::HandleServerShutdown() {
    if (closed_) {
        return;
    }

    const std::optional<std::string> inflight_turn_id = active_turn_id();
    VLOG(1) << "AI gateway session=" << session_id_ << " closed for server shutdown"
            << " session_started="
            << (handler_.snapshot().status != isla::shared::ai_gateway::SessionStatus::NotStarted
                    ? "true"
                    : "false")
            << " inflight_turn_id='"
            << (inflight_turn_id.has_value() ? SanitizeForLog(*inflight_turn_id)
                                             : std::string("<none>"))
            << "'";
    CloseSession(SessionCloseReason::ServerStopping, "server stopping", inflight_turn_id, false);
}

absl::Status GatewayWebSocketSessionAdapter::SendFrames(const std::vector<std::string>& frames) {
    for (const std::string& frame : frames) {
        const absl::Status status = connection_.SendTextFrame(frame);
        if (!status.ok()) {
            LOG(WARNING) << "AI gateway session=" << session_id_
                         << " failed to send websocket text frame: " << status;
            CloseSession(SessionCloseReason::SendFailed, status.message(), active_turn_id(), true);
            return status;
        }
    }
    return absl::OkStatus();
}

void GatewayWebSocketSessionAdapter::CloseSession(SessionCloseReason reason,
                                                  std::string_view detail,
                                                  std::optional<std::string> inflight_turn_id,
                                                  bool close_transport) {
    if (closed_) {
        return;
    }

    closed_ = true;
    if (close_transport) {
        connection_.Close();
    }
    VLOG(1) << "AI gateway session=" << session_id_
            << " closed reason=" << close_reason_name(reason) << " session_started="
            << (handler_.snapshot().status != isla::shared::ai_gateway::SessionStatus::NotStarted
                    ? "true"
                    : "false")
            << " inflight_turn_id='"
            << (inflight_turn_id.has_value() ? SanitizeForLog(*inflight_turn_id)
                                             : std::string("<none>"))
            << "' detail='" << SanitizeForLog(detail) << "'";
    if (event_sink_ != nullptr) {
        event_sink_->OnSessionClosed(SessionClosedEvent{
            .session_id = session_id_,
            .session_started =
                handler_.snapshot().status != isla::shared::ai_gateway::SessionStatus::NotStarted,
            .inflight_turn_id = std::move(inflight_turn_id),
            .reason = reason,
            .detail = std::string(detail),
        });
    }
}

std::optional<std::string> GatewayWebSocketSessionAdapter::active_turn_id() const {
    if (!handler_.snapshot().active_turn.has_value()) {
        return std::nullopt;
    }
    return handler_.snapshot().active_turn->turn_id;
}

GatewayWebSocketSessionFactory::GatewayWebSocketSessionFactory(
    std::unique_ptr<SessionIdGenerator> session_id_generator)
    : session_id_generator_(std::move(session_id_generator)) {}

std::unique_ptr<GatewayWebSocketSessionAdapter>
GatewayWebSocketSessionFactory::CreateSession(GatewayWebSocketConnection& connection,
                                              GatewaySessionEventSink* event_sink) {
    return std::make_unique<GatewayWebSocketSessionAdapter>(session_id_generator_->NextSessionId(),
                                                            connection, event_sink);
}

} // namespace isla::server::ai_gateway
