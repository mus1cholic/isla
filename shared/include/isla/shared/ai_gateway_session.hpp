#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"

namespace isla::shared::ai_gateway {

enum class SessionStatus {
    NotStarted = 0,
    Active,
    Ended,
};

enum class TurnStatus {
    InFlight = 0,
    CancelRequested,
};

struct TurnState {
    std::string turn_id;
    TurnStatus status = TurnStatus::InFlight;
    bool text_output_emitted = false;
    bool audio_output_emitted = false;
};

struct SessionSnapshot {
    SessionStatus status = SessionStatus::NotStarted;
    std::string session_id;
    std::optional<TurnState> active_turn;
};

class SessionState {
  public:
    [[nodiscard]] absl::Status start(std::string_view session_id);
    [[nodiscard]] absl::Status begin_turn(std::string_view turn_id);
    [[nodiscard]] absl::Status mark_text_output(std::string_view turn_id);
    [[nodiscard]] absl::Status mark_audio_output(std::string_view turn_id);
    [[nodiscard]] absl::Status complete_turn(std::string_view turn_id);
    [[nodiscard]] absl::Status request_turn_cancel(std::string_view turn_id);
    [[nodiscard]] absl::Status confirm_turn_cancel(std::string_view turn_id);
    [[nodiscard]] absl::Status end();

    [[nodiscard]] const SessionSnapshot& snapshot() const {
        return snapshot_;
    }

  private:
    SessionSnapshot snapshot_{};
};

} // namespace isla::shared::ai_gateway
