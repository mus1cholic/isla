#include "isla/shared/ai_gateway_session.hpp"

#include <string>
#include <string_view>

namespace isla::shared::ai_gateway {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

bool matches_active_turn(const SessionSnapshot& snapshot, std::string_view turn_id) {
    return snapshot.active_turn.has_value() && snapshot.active_turn->turn_id == turn_id;
}

} // namespace

absl::Status SessionState::start(std::string_view session_id) {
    if (session_id.empty()) {
        return invalid_argument("session_id must be non-empty");
    }
    if (snapshot_.status != SessionStatus::NotStarted) {
        return failed_precondition("session is already started or ended");
    }

    snapshot_.status = SessionStatus::Active;
    snapshot_.session_id = std::string(session_id);
    snapshot_.active_turn.reset();
    return absl::OkStatus();
}

absl::Status SessionState::begin_turn(std::string_view turn_id) {
    if (snapshot_.status != SessionStatus::Active) {
        return failed_precondition("session is not active");
    }
    if (turn_id.empty()) {
        return invalid_argument("turn_id must be non-empty");
    }
    if (snapshot_.active_turn.has_value()) {
        return failed_precondition("only one turn may be in flight per session");
    }

    snapshot_.active_turn = TurnState{ std::string(turn_id), TurnStatus::InFlight, false, false };
    return absl::OkStatus();
}

absl::Status SessionState::mark_text_output(std::string_view turn_id) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return failed_precondition("turn_id does not match the active turn");
    }
    if (snapshot_.active_turn->text_output_emitted) {
        return failed_precondition("text output already emitted for active turn");
    }

    snapshot_.active_turn->text_output_emitted = true;
    return absl::OkStatus();
}

absl::Status SessionState::mark_audio_output(std::string_view turn_id) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return failed_precondition("turn_id does not match the active turn");
    }
    if (!snapshot_.active_turn->text_output_emitted) {
        return failed_precondition("audio output requires text output first");
    }
    if (snapshot_.active_turn->audio_output_emitted) {
        return failed_precondition("audio output already emitted for active turn");
    }

    snapshot_.active_turn->audio_output_emitted = true;
    return absl::OkStatus();
}

absl::Status SessionState::complete_turn(std::string_view turn_id) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return failed_precondition("turn_id does not match the active turn");
    }

    snapshot_.active_turn.reset();
    return absl::OkStatus();
}

absl::Status SessionState::request_turn_cancel(std::string_view turn_id) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return failed_precondition("turn_id does not match the active turn");
    }
    if (snapshot_.active_turn->status == TurnStatus::CancelRequested) {
        return failed_precondition("turn cancellation already requested");
    }

    snapshot_.active_turn->status = TurnStatus::CancelRequested;
    return absl::OkStatus();
}

absl::Status SessionState::confirm_turn_cancel(std::string_view turn_id) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return failed_precondition("turn_id does not match the active turn");
    }
    if (snapshot_.active_turn->status != TurnStatus::CancelRequested) {
        return failed_precondition("turn cancellation was not requested");
    }

    snapshot_.active_turn.reset();
    return absl::OkStatus();
}

absl::Status SessionState::end() {
    if (snapshot_.status != SessionStatus::Active) {
        return failed_precondition("session is not active");
    }
    if (snapshot_.active_turn.has_value()) {
        return failed_precondition("session cannot end while a turn is in flight");
    }

    snapshot_.status = SessionStatus::Ended;
    return absl::OkStatus();
}

} // namespace isla::shared::ai_gateway
