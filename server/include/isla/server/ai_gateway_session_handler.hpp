#pragma once

// ai_gateway_session_handler.hpp
//
// Pure protocol-level state machine for a single gateway WebSocket session.
//
// GatewaySessionHandler is the translation layer between raw client JSON frames
// and the structured events consumed by the application layer
// (GatewayApplicationEventSink / the stub responder). It owns no I/O, no
// threads, and no transport state — callers feed it one inbound frame at a
// time, and it returns:
//   * outgoing JSON frames that must be written back to the client, and
//   * optional high-level events (session start, transcript seed, turn accept,
//     turn cancel) that the transport layer routes to the application sink.
//
// The class wraps an isla::shared::ai_gateway::SessionState and is responsible
// for enforcing the protocol FSM (which messages are valid in which state),
// validating payload shapes and size limits, and producing well-formed
// response/error frames on behalf of the transport.
//
// This header is safe to include from unit tests that want to exercise the
// protocol logic in isolation without spinning up a real WebSocket server.

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

// Maximum size, in bytes, of a single inbound text payload (text.input /
// transcript.seed). Requests exceeding this are rejected with a bad_request
// error before any application-layer work is done.
inline constexpr std::size_t kMaxTextInputBytes = 32U * 1024U;
// Maximum size, in bytes, of a single outbound text.output frame. The
// application layer is expected to chunk longer responses across multiple
// EmitTextOutput() calls.
inline constexpr std::size_t kMaxTextOutputBytes = 32U * 1024U;

// Event emitted when a client text.input message has been validated and
// accepted by the protocol FSM. The transport layer hands this to the
// application sink so it can start generating a response. `telemetry_context`
// is pre-populated with the turn's accept timestamp and is expected to be
// propagated through every downstream LLM / embedding / memory call for that
// turn so latency is correctly attributed.
struct TurnAcceptedEvent {
    std::string session_id;
    std::string turn_id;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
    std::shared_ptr<const TurnTelemetryContext> telemetry_context;
};

// Startup payload for a session. The same struct is reused for two distinct
// events: the pre-start request (where the application sink decides whether
// to accept the session) and the post-start notification (where the sink is
// told the `session.started` frame has been emitted and the session is Active).
// See the SessionStartRequestEvent / SessionStartedEvent aliases below.
struct SessionStartInfo {
    std::string session_id;
    std::string user_id;
    // Wall-clock time the client claims the session began. Optional because
    // older clients may not send it; callers should fall back to server time.
    std::optional<isla::server::memory::Timestamp> session_start_time;
    // Reference time used for evaluation runs that replay historical sessions —
    // lets benchmarks pin "now" to a recorded timestamp. Normal production
    // clients leave this unset.
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
};

// Fires on receipt of a client `session.start` frame, before the application
// sink has decided whether to accept the session. The application sink's
// decision is reported back via GatewaySessionHandler::AcceptSessionStart() or
// RejectSessionStart().
using SessionStartRequestEvent = SessionStartInfo;
// Fires after the server has sent `session.started` and the FSM has
// transitioned to Active. Used by the application sink to finalize any
// post-accept setup that depends on the session being live.
using SessionStartedEvent = SessionStartInfo;

// Event emitted when a client `turn.cancel` message has been accepted. The
// application sink is expected to abort any in-flight work for `turn_id` and
// emit a `turn.cancelled` frame once cleanup has completed.
struct TurnCancelRequestedEvent {
    std::string session_id;
    std::string turn_id;
};

// Event emitted when a client `transcript.seed` message has been accepted.
// Used to backfill conversation history without invoking the LLM; the
// application sink persists the message into working memory and then confirms
// via EmitTranscriptSeeded(). Valid only outside an active turn on an Active
// session (the FSM enforces this).
struct TranscriptSeedEvent {
    std::string session_id;
    std::string turn_id;
    std::string role;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
};

// Outcome of a single HandleIncomingJson() call. Designed so the transport
// layer can apply it without any further parsing:
//   1. If `!ok`, the frame was rejected (`error_message` explains why) and
//      `outgoing_frames` contains a pre-encoded error frame to send back.
//   2. If `ok`, write any `outgoing_frames` to the client.
//   3. If any of the optional event fields is populated, dispatch it to the
//      application sink. At most one of the event fields will be set for a
//      given inbound frame.
//   4. If `should_close`, the server should close the connection after the
//      outgoing frames have flushed (this fires on `session.end`).
// `error_message` is populated on rejection and is primarily a logging aid —
// the structured error has already been encoded into `outgoing_frames`.
struct HandleIncomingResult {
    bool ok = false;
    std::vector<std::string> outgoing_frames;
    std::optional<SessionStartRequestEvent> session_start_requested;
    std::optional<TranscriptSeedEvent> transcript_seed;
    std::optional<TurnAcceptedEvent> accepted_turn;
    std::optional<TurnCancelRequestedEvent> cancel_requested;
    bool should_close = false;
    std::string error_message;
};

// Outcome of an Emit* call. Each Emit* method produces zero or more fully
// encoded JSON frames that the transport layer writes back to the client in
// order. Callers must not reorder frames within a single EmitResult.
struct EmitResult {
    std::vector<std::string> outgoing_frames;
};

// Pure protocol-level handler for a single gateway WebSocket session. Owns
// the session's FSM (wrapping isla::shared::ai_gateway::SessionState) and
// validates/encodes frames on behalf of the transport layer. One instance per
// live session; instances are NOT thread-safe — the transport is expected to
// serialize all calls onto a single session-owned executor (the adapter in
// ai_gateway_websocket_session.cpp handles this).
//
// All methods return either a HandleIncomingResult or an absl::StatusOr<EmitResult>.
// Returning a non-OK status from an Emit* call means the caller tried to emit
// from an invalid FSM state (e.g. EmitTextOutput for a turn that isn't
// active) — these should be treated as server bugs, not client errors, and
// generally logged loudly.
class GatewaySessionHandler {
  public:
    // Constructs a handler for a session with the given id. The telemetry
    // sink is used to record per-turn phase timings (gateway-accept latency,
    // turn-accepted events); pass CreateNoOpTelemetrySink() in tests where
    // telemetry output is not asserted on.
    explicit GatewaySessionHandler(
        std::string session_id,
        std::shared_ptr<const TelemetrySink> telemetry_sink = CreateNoOpTelemetrySink());

    // Parses one inbound JSON frame from the client, validates it against the
    // current FSM state, and returns a HandleIncomingResult describing what
    // the transport should do next. This is the single entry point for client
    // messages — the transport hands every received frame through here.
    //
    // On success, exactly one of the result's event fields may be populated
    // (session_start_requested, transcript_seed, accepted_turn,
    // cancel_requested); the transport layer forwards that to the application
    // sink. On failure, `ok` is false, `error_message` explains why, and
    // `outgoing_frames` already contains a pre-encoded `error` frame to send
    // back to the client — no additional error emission is required.
    //
    // Never throws. Malformed JSON, unknown message types, size-limit
    // violations, and FSM-order violations (e.g. `text.input` before
    // `session.start` has completed) are all mapped to rejection results.
    [[nodiscard]] HandleIncomingResult HandleIncomingJson(std::string_view json_text);

    // Transitions the FSM from Starting → Active and produces the outgoing
    // `session.started` frame. Called by the transport after the application
    // sink has accepted the SessionStartRequestEvent. Returns a FailedPrecondition
    // if the FSM is not currently in the Starting state.
    [[nodiscard]] absl::StatusOr<EmitResult> AcceptSessionStart();

    // Transitions the FSM from Starting → Failed and produces an outgoing
    // `error` frame describing why the session was rejected. Called by the
    // transport after the application sink has rejected the SessionStartRequestEvent.
    // Returns a FailedPrecondition if the FSM is not currently in the Starting state.
    [[nodiscard]] absl::StatusOr<EmitResult> RejectSessionStart(std::string_view code,
                                                                std::string_view message);

    // Produces an outgoing `text.output` frame for an active turn and records
    // that text output has been sent for the turn (used by the FSM to decide
    // whether a subsequent `turn.completed` is valid). Rejects empty text,
    // text exceeding kMaxTextOutputBytes, and writes for turns that are not
    // currently active.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTextOutput(std::string_view turn_id,
                                                            std::string_view text);

    // Produces an outgoing `audio.output` frame for an active turn. `audio_base64`
    // must already be base64-encoded; this method does not re-encode. Rejects
    // empty mime_type or audio payload and writes for turns that are not
    // currently active.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitAudioOutput(std::string_view turn_id,
                                                             std::string_view mime_type,
                                                             std::string_view audio_base64);

    // Produces an outgoing `transcript.seeded` confirmation frame for a
    // transcript-seed request the application sink has just persisted. This
    // method is `const` because it does not mutate the FSM — it only gates
    // on the current session being Active. Rejects empty turn_id or role and
    // emission outside an Active session.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTranscriptSeeded(std::string_view turn_id,
                                                                  std::string_view role) const;

    // Transitions the active turn to Completed and produces an outgoing
    // `turn.completed` frame. Called after the application sink has finished
    // streaming outputs for the turn. Returns a FailedPrecondition if the
    // turn is not the currently active turn.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTurnCompleted(std::string_view turn_id);

    // Confirms an in-flight cancellation and produces an outgoing
    // `turn.cancelled` frame. Called by the transport after the application
    // sink has finished aborting work in response to a TurnCancelRequestedEvent.
    // Returns a FailedPrecondition if the turn is not in a cancellable state.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitTurnCancelled(std::string_view turn_id);

    // Produces an out-of-band `error` frame. Used for application-layer
    // failures that occur outside the normal request/response flow (e.g. LLM
    // provider outage during a streaming turn). `turn_id` may be null for
    // session-scoped errors. Does not mutate the FSM, hence `const`.
    // The outgoing frame's session_id is omitted if the session has not yet
    // reached Active, so clients cannot correlate pre-start errors with a
    // session that never existed. Rejects empty code or message.
    [[nodiscard]] absl::StatusOr<EmitResult> EmitError(std::optional<std::string_view> turn_id,
                                                       std::string_view code,
                                                       std::string_view message) const;

    // Read-only view of the underlying session state. Primarily intended for
    // tests and diagnostic logging; production code should not branch on the
    // snapshot in place of using the explicit FSM transitions above.
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
