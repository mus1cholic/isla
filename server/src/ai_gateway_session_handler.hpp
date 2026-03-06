#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "isla/shared/ai_gateway_protocol.hpp"
#include "isla/shared/ai_gateway_session.hpp"

namespace isla::server::ai_gateway {

struct TurnAcceptedEvent {
    std::string session_id;
    std::string turn_id;
    std::string text;
};

struct TurnCancelRequestedEvent {
    std::string session_id;
    std::string turn_id;
};

struct HandleIncomingResult {
    bool ok = false;
    std::vector<std::string> outgoing_frames;
    std::optional<TurnAcceptedEvent> accepted_turn;
    std::optional<TurnCancelRequestedEvent> cancel_requested;
    bool should_close = false;
    std::string error_message;
};

struct EmitResult {
    bool ok = false;
    std::vector<std::string> outgoing_frames;
    std::string error_message;
};

class GatewaySessionHandler {
  public:
    explicit GatewaySessionHandler(std::string session_id = "srv_1");

    [[nodiscard]] HandleIncomingResult HandleIncomingJson(std::string_view json_text);
    [[nodiscard]] EmitResult EmitTextOutput(std::string_view turn_id, std::string_view text);
    [[nodiscard]] EmitResult EmitAudioOutput(std::string_view turn_id,
                                            std::string_view mime_type,
                                            std::string_view audio_base64);
    [[nodiscard]] EmitResult EmitTurnCompleted(std::string_view turn_id);
    [[nodiscard]] EmitResult EmitTurnCancelled(std::string_view turn_id);
    [[nodiscard]] EmitResult EmitError(std::optional<std::string_view> turn_id,
                                       std::string_view code,
                                       std::string_view message) const;

    [[nodiscard]] const isla::shared::ai_gateway::SessionSnapshot& snapshot() const {
        return session_state_.snapshot();
    }

  private:
    [[nodiscard]] HandleIncomingResult RejectIncoming(std::optional<std::string_view> turn_id,
                                                      std::string_view code,
                                                      std::string_view message) const;
    [[nodiscard]] std::string current_session_id() const;
    [[nodiscard]] std::string encode(
        const isla::shared::ai_gateway::GatewayMessage& message) const;

    std::string session_id_;
    isla::shared::ai_gateway::SessionState session_state_{};
};

} // namespace isla::server::ai_gateway
