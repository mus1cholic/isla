#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/shared/ai_gateway_protocol.hpp"
#include "isla/shared/ai_gateway_session.hpp"

namespace isla::server::ai_gateway {

inline constexpr std::size_t kMaxTextInputBytes = 32U * 1024U;
inline constexpr std::size_t kMaxTextOutputBytes = 32U * 1024U;

struct TurnAcceptedEvent {
    std::string session_id;
    std::string turn_id;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
    std::shared_ptr<const TurnTelemetryContext> telemetry_context;
};

struct SessionStartedEvent {
    std::string session_id;
    std::string user_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
};

struct TurnCancelRequestedEvent {
    std::string session_id;
    std::string turn_id;
};

struct TranscriptSeedEvent {
    std::string session_id;
    std::string turn_id;
    std::string role;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
};

struct HandleIncomingResult {
    bool ok = false;
    std::vector<std::string> outgoing_frames;
    std::optional<SessionStartedEvent> session_started;
    std::optional<TranscriptSeedEvent> transcript_seed;
    std::optional<TurnAcceptedEvent> accepted_turn;
    std::optional<TurnCancelRequestedEvent> cancel_requested;
    bool should_close = false;
    std::string error_message;
};

struct EmitResult {
    std::vector<std::string> outgoing_frames;
};

class GatewaySessionHandler {
  public:
    explicit GatewaySessionHandler(
        std::string session_id,
        std::shared_ptr<const TelemetrySink> telemetry_sink = CreateNoOpTelemetrySink());

    [[nodiscard]] HandleIncomingResult HandleIncomingJson(std::string_view json_text);
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTextOutput(std::string_view turn_id,
                                                            std::string_view text);
    [[nodiscard]] absl::StatusOr<EmitResult> EmitAudioOutput(std::string_view turn_id,
                                                             std::string_view mime_type,
                                                             std::string_view audio_base64);
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTranscriptSeeded(std::string_view turn_id,
                                                                  std::string_view role) const;
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTurnCompleted(std::string_view turn_id);
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTurnCancelled(std::string_view turn_id);
    [[nodiscard]] absl::StatusOr<EmitResult> EmitError(std::optional<std::string_view> turn_id,
                                                       std::string_view code,
                                                       std::string_view message) const;

    [[nodiscard]] const isla::shared::ai_gateway::SessionSnapshot& snapshot() const {
        return session_state_.snapshot();
    }

  private:
    [[nodiscard]] HandleIncomingResult RejectIncoming(std::optional<std::string_view> turn_id,
                                                      std::string_view code,
                                                      std::string_view message) const;
    [[nodiscard]] const std::string& current_session_id() const;
    [[nodiscard]] std::string encode(const isla::shared::ai_gateway::GatewayMessage& message) const;

    std::string session_id_;
    std::shared_ptr<const TelemetrySink> telemetry_sink_;
    isla::shared::ai_gateway::SessionState session_state_{};
};

} // namespace isla::server::ai_gateway
