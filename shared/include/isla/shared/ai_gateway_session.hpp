#pragma once

#include <optional>
#include <string>
#include <string_view>

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
    [[nodiscard]] bool start(std::string_view session_id, std::string* error_message = nullptr);
    [[nodiscard]] bool begin_turn(std::string_view turn_id, std::string* error_message = nullptr);
    [[nodiscard]] bool mark_text_output(std::string_view turn_id,
                                        std::string* error_message = nullptr);
    [[nodiscard]] bool mark_audio_output(std::string_view turn_id,
                                         std::string* error_message = nullptr);
    [[nodiscard]] bool complete_turn(std::string_view turn_id, std::string* error_message = nullptr);
    [[nodiscard]] bool request_turn_cancel(std::string_view turn_id,
                                           std::string* error_message = nullptr);
    [[nodiscard]] bool confirm_turn_cancel(std::string_view turn_id,
                                           std::string* error_message = nullptr);
    [[nodiscard]] bool end(std::string* error_message = nullptr);

    [[nodiscard]] const SessionSnapshot& snapshot() const {
        return snapshot_;
    }

  private:
    SessionSnapshot snapshot_{};
};

} // namespace isla::shared::ai_gateway
