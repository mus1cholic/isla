#include "isla/shared/ai_gateway_session.hpp"

#include <string>
#include <string_view>

namespace isla::shared::ai_gateway {
namespace {

bool fail(std::string_view message, std::string* error_message) {
    if (error_message != nullptr) {
        *error_message = std::string(message);
    }
    return false;
}

bool matches_active_turn(const SessionSnapshot& snapshot, std::string_view turn_id) {
    return snapshot.active_turn.has_value() && snapshot.active_turn->turn_id == turn_id;
}

} // namespace

bool SessionState::start(std::string_view session_id, std::string* error_message) {
    if (session_id.empty()) {
        return fail("session_id must be non-empty", error_message);
    }
    if (snapshot_.status != SessionStatus::NotStarted) {
        return fail("session is already started or ended", error_message);
    }

    snapshot_.status = SessionStatus::Active;
    snapshot_.session_id = std::string(session_id);
    snapshot_.active_turn.reset();
    return true;
}

bool SessionState::begin_turn(std::string_view turn_id, std::string* error_message) {
    if (snapshot_.status != SessionStatus::Active) {
        return fail("session is not active", error_message);
    }
    if (turn_id.empty()) {
        return fail("turn_id must be non-empty", error_message);
    }
    if (snapshot_.active_turn.has_value()) {
        return fail("only one turn may be in flight per session", error_message);
    }

    snapshot_.active_turn = TurnState{std::string(turn_id), TurnStatus::InFlight, false, false};
    return true;
}

bool SessionState::mark_text_output(std::string_view turn_id, std::string* error_message) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return fail("turn_id does not match the active turn", error_message);
    }
    if (snapshot_.active_turn->text_output_emitted) {
        return fail("text output already emitted for active turn", error_message);
    }

    snapshot_.active_turn->text_output_emitted = true;
    return true;
}

bool SessionState::mark_audio_output(std::string_view turn_id, std::string* error_message) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return fail("turn_id does not match the active turn", error_message);
    }
    if (!snapshot_.active_turn->text_output_emitted) {
        return fail("audio output requires text output first", error_message);
    }
    if (snapshot_.active_turn->audio_output_emitted) {
        return fail("audio output already emitted for active turn", error_message);
    }

    snapshot_.active_turn->audio_output_emitted = true;
    return true;
}

bool SessionState::complete_turn(std::string_view turn_id, std::string* error_message) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return fail("turn_id does not match the active turn", error_message);
    }

    snapshot_.active_turn.reset();
    return true;
}

bool SessionState::request_turn_cancel(std::string_view turn_id, std::string* error_message) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return fail("turn_id does not match the active turn", error_message);
    }
    if (snapshot_.active_turn->status == TurnStatus::CancelRequested) {
        return fail("turn cancellation already requested", error_message);
    }

    snapshot_.active_turn->status = TurnStatus::CancelRequested;
    return true;
}

bool SessionState::confirm_turn_cancel(std::string_view turn_id, std::string* error_message) {
    if (!matches_active_turn(snapshot_, turn_id)) {
        return fail("turn_id does not match the active turn", error_message);
    }
    if (snapshot_.active_turn->status != TurnStatus::CancelRequested) {
        return fail("turn cancellation was not requested", error_message);
    }

    snapshot_.active_turn.reset();
    return true;
}

bool SessionState::end(std::string* error_message) {
    if (snapshot_.status != SessionStatus::Active) {
        return fail("session is not active", error_message);
    }
    if (snapshot_.active_turn.has_value()) {
        return fail("session cannot end while a turn is in flight", error_message);
    }

    snapshot_.status = SessionStatus::Ended;
    return true;
}

} // namespace isla::shared::ai_gateway
