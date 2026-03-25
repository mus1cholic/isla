#include "isla/server/ai_gateway_session_handler.hpp"

#include <memory>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace protocol = isla::shared::ai_gateway;

absl::StatusOr<protocol::GatewayMessage> parse_frame(const std::string& frame) {
    return protocol::parse_json_message(frame);
}

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    struct EventRecord {
        std::string name;
    };

    struct PhaseRecord {
        std::string name;
        TurnTelemetryContext::Clock::time_point started_at;
        TurnTelemetryContext::Clock::time_point completed_at;
    };

    void OnTurnAccepted(const TurnTelemetryContext& context) const override {
        accepted_turns.push_back({ .session_id = context.session_id, .turn_id = context.turn_id });
    }

    void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at) const override {
        static_cast<void>(context);
        static_cast<void>(at);
        events.push_back(EventRecord{ .name = std::string(event_name) });
    }

    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        static_cast<void>(context);
        phases.push_back(PhaseRecord{
            .name = std::string(phase_name),
            .started_at = started_at,
            .completed_at = completed_at,
        });
    }

    mutable std::vector<TurnAcceptedEvent> accepted_turns;
    mutable std::vector<EventRecord> events;
    mutable std::vector<PhaseRecord> phases;
};

TEST(AiGatewaySessionHandlerTest, SessionStartEmitsSessionStarted) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.start"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    ASSERT_TRUE(result.session_started.has_value());
    EXPECT_EQ(result.session_started->session_id, "srv_test");
    ASSERT_FALSE(result.accepted_turn.has_value());

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*frame));
    EXPECT_EQ(std::get<protocol::SessionStartedMessage>(*frame).session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, SessionStartParsesOptionalReplayTimestamps) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"session.start","session_start_time":"2026-03-14T09:59:00Z","evaluation_reference_time":"2026-03-20T08:00:00Z"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.session_started.has_value());
    ASSERT_TRUE(result.session_started->session_start_time.has_value());
    ASSERT_TRUE(result.session_started->evaluation_reference_time.has_value());
    EXPECT_EQ(*result.session_started->session_start_time,
              isla::server::memory::ParseTimestamp("2026-03-14T09:59:00Z"));
    EXPECT_EQ(*result.session_started->evaluation_reference_time,
              isla::server::memory::ParseTimestamp("2026-03-20T08:00:00Z"));
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateSessionStart) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.start"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.session_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, RejectsTextInputBeforeSessionStart) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_FALSE(error.session_id.has_value());
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_1");
}

TEST(AiGatewaySessionHandlerTest, AcceptsTextInputAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json");

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.outgoing_frames.empty());
    ASSERT_TRUE(result.accepted_turn.has_value());
    EXPECT_EQ(result.accepted_turn->session_id, "srv_test");
    EXPECT_EQ(result.accepted_turn->turn_id, "turn_1");
    EXPECT_EQ(result.accepted_turn->text, "hello");
    ASSERT_NE(result.accepted_turn->telemetry_context, nullptr);
    EXPECT_EQ(result.accepted_turn->telemetry_context->session_id, "srv_test");
    EXPECT_EQ(result.accepted_turn->telemetry_context->turn_id, "turn_1");
}

TEST(AiGatewaySessionHandlerTest, AcceptsTextInputCreateTimeAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello","create_time":"2026-03-15T11:30:00Z"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.accepted_turn.has_value());
    ASSERT_TRUE(result.accepted_turn->create_time.has_value());
    EXPECT_EQ(*result.accepted_turn->create_time,
              isla::server::memory::ParseTimestamp("2026-03-15T11:30:00Z"));
}

TEST(AiGatewaySessionHandlerTest, AcceptsTranscriptSeedAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"transcript.seed","turn_id":"turn_seed","role":"assistant","text":"seeded context"})json");

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.outgoing_frames.empty());
    ASSERT_TRUE(result.transcript_seed.has_value());
    EXPECT_EQ(result.transcript_seed->session_id, "srv_test");
    EXPECT_EQ(result.transcript_seed->turn_id, "turn_seed");
    EXPECT_EQ(result.transcript_seed->role, "assistant");
    EXPECT_EQ(result.transcript_seed->text, "seeded context");
    EXPECT_FALSE(result.accepted_turn.has_value());
}

TEST(AiGatewaySessionHandlerTest, AcceptsTranscriptSeedCreateTimeAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"transcript.seed","turn_id":"turn_seed","role":"assistant","text":"seeded context","create_time":"2026-03-14T10:00:05Z"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.transcript_seed.has_value());
    ASSERT_TRUE(result.transcript_seed->create_time.has_value());
    EXPECT_EQ(*result.transcript_seed->create_time,
              isla::server::memory::ParseTimestamp("2026-03-14T10:00:05Z"));
}

TEST(AiGatewaySessionHandlerTest, AcceptedTurnNotifiesCustomTelemetrySink) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    GatewaySessionHandler handler("srv_test", telemetry_sink);
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.accepted_turn.has_value());
    ASSERT_NE(result.accepted_turn->telemetry_context, nullptr);
    EXPECT_EQ(result.accepted_turn->telemetry_context->sink, telemetry_sink);
    ASSERT_EQ(telemetry_sink->accepted_turns.size(), 1U);
    EXPECT_EQ(telemetry_sink->accepted_turns.front().session_id, "srv_test");
    EXPECT_EQ(telemetry_sink->accepted_turns.front().turn_id, "turn_1");
    ASSERT_EQ(telemetry_sink->events.size(), 1U);
    EXPECT_EQ(telemetry_sink->events.front().name, telemetry::kEventTurnAccepted);
    ASSERT_EQ(telemetry_sink->phases.size(), 1U);
    EXPECT_EQ(telemetry_sink->phases.front().name, telemetry::kPhaseGatewayAccept);
    EXPECT_LE(telemetry_sink->phases.front().started_at,
              telemetry_sink->phases.front().completed_at);
}

TEST(AiGatewaySessionHandlerTest, RejectsConcurrentSecondTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_2","text":"again"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_2");
}

TEST(AiGatewaySessionHandlerTest, RejectsTranscriptSeedWhileLiveTurnIsActive) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"transcript.seed","turn_id":"turn_seed","role":"user","text":"blocked"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_seed");
    EXPECT_EQ(error.message, "transcript.seed is not allowed while a live turn is active");
}

TEST(AiGatewaySessionHandlerTest, RejectsOversizedTextInputBeforeTurnAcceptance) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const std::string oversized_text(kMaxTextInputBytes + 1U, 'x');
    const std::string json =
        std::string("{\"type\":\"text.input\",\"turn_id\":\"turn_1\",\"text\":\"") +
        oversized_text + "\"}";

    const HandleIncomingResult result = handler.HandleIncomingJson(json);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.accepted_turn.has_value());
    EXPECT_FALSE(handler.snapshot().active_turn.has_value());
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_EQ(error.code, "bad_request");
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_1");
    EXPECT_EQ(error.message, "text.input text exceeds maximum length");
}

TEST(AiGatewaySessionHandlerTest, EmitsTranscriptSeededAcknowledgement) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const absl::StatusOr<EmitResult> result =
        handler.EmitTranscriptSeeded("turn_seed", "assistant");

    ASSERT_TRUE(result.ok()) << result.status().ToString();
    ASSERT_EQ(result->outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result->outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TranscriptSeededMessage>(*frame));
    const auto& seeded = std::get<protocol::TranscriptSeededMessage>(*frame);
    EXPECT_EQ(seeded.turn_id, "turn_seed");
    EXPECT_EQ(seeded.role, "assistant");
}

TEST(AiGatewaySessionHandlerTest, EmitsTextAudioAndCompletionForAcceptedTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const absl::StatusOr<EmitResult> text = handler.EmitTextOutput("turn_1", "hi");
    const absl::StatusOr<EmitResult> audio =
        handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");
    const absl::StatusOr<EmitResult> completed = handler.EmitTurnCompleted("turn_1");

    ASSERT_TRUE(text.ok()) << text.status().ToString();
    ASSERT_TRUE(audio.ok()) << audio.status().ToString();
    ASSERT_TRUE(completed.ok()) << completed.status().ToString();
    ASSERT_EQ(text->outgoing_frames.size(), 1U);
    ASSERT_EQ(audio->outgoing_frames.size(), 1U);
    ASSERT_EQ(completed->outgoing_frames.size(), 1U);
    EXPECT_FALSE(handler.snapshot().active_turn.has_value());

    const absl::StatusOr<protocol::GatewayMessage> text_frame =
        parse_frame(text->outgoing_frames.front());
    ASSERT_TRUE(text_frame.ok()) << text_frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*text_frame));

    const absl::StatusOr<protocol::GatewayMessage> audio_frame =
        parse_frame(audio->outgoing_frames.front());
    ASSERT_TRUE(audio_frame.ok()) << audio_frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::AudioOutputMessage>(*audio_frame));

    const absl::StatusOr<protocol::GatewayMessage> completed_frame =
        parse_frame(completed->outgoing_frames.front());
    ASSERT_TRUE(completed_frame.ok()) << completed_frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*completed_frame));
}

TEST(AiGatewaySessionHandlerTest, RejectsOversizedTextOutput) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const absl::StatusOr<EmitResult> result =
        handler.EmitTextOutput("turn_1", std::string(kMaxTextOutputBytes + 1U, 'x'));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(result.status().message(), "text output exceeds maximum length");
}

TEST(AiGatewaySessionHandlerTest, RejectsAudioBeforeText) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const absl::StatusOr<EmitResult> result =
        handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "audio output requires text output first");
}

TEST(AiGatewaySessionHandlerTest, SurfacesTurnCancelAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult cancel =
        handler.HandleIncomingJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json");
    ASSERT_TRUE(cancel.ok);
    ASSERT_TRUE(cancel.cancel_requested.has_value());
    EXPECT_EQ(cancel.cancel_requested->session_id, "srv_test");
    EXPECT_EQ(cancel.cancel_requested->turn_id, "turn_1");

    const absl::StatusOr<EmitResult> cancelled = handler.EmitTurnCancelled("turn_1");
    ASSERT_TRUE(cancelled.ok()) << cancelled.status().ToString();
    EXPECT_FALSE(handler.snapshot().active_turn.has_value());
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(cancelled->outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::TurnCancelledMessage>(*frame));
}

TEST(AiGatewaySessionHandlerTest, RejectsTurnCancelForUnknownTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"turn.cancel","turn_id":"turn_missing"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_missing");
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateTurnCancelRequest) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(
        handler.HandleIncomingJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_1");
}

TEST(AiGatewaySessionHandlerTest, RejectsEmitTurnCancelledWithoutCancelRequest) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const absl::StatusOr<EmitResult> result = handler.EmitTurnCancelled("turn_1");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "turn cancellation was not requested");
}

TEST(AiGatewaySessionHandlerTest, RejectsServerOwnedInboundMessages) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"transcript.seeded","turn_id":"turn_1","role":"assistant"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
}

TEST(AiGatewaySessionHandlerTest, RejectsSessionEndWhileTurnIsActive) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_test"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
}

TEST(AiGatewaySessionHandlerTest, RejectsSessionEndWithMismatchedSessionId) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_other"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.session_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, SessionEndEmitsSessionEndedAfterTurnCompletes) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok());
    ASSERT_TRUE(handler.EmitTurnCompleted("turn_1").ok());

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_test"})json");

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.should_close);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    EXPECT_EQ(handler.snapshot().status, protocol::SessionStatus::Ended);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*frame));
}

TEST(AiGatewaySessionHandlerTest, ParseFailureReturnsErrorFrame) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson("{not valid json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_FALSE(error.session_id.has_value());
    EXPECT_EQ(error.code, "bad_request");
}

TEST(AiGatewaySessionHandlerTest, InvalidReplayTimestampReturnsErrorFrame) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"session.start","session_start_time":"not-a-timestamp"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result.outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_EQ(error.code, "bad_request");
    EXPECT_NE(error.message.find("session_start_time is not a valid timestamp"), std::string::npos);
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateTextOutput) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok());

    const absl::StatusOr<EmitResult> result = handler.EmitTextOutput("turn_1", "hi again");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "text output already emitted for active turn");
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateAudioOutput) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok());
    ASSERT_TRUE(handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==").ok());

    const absl::StatusOr<EmitResult> result =
        handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "audio output already emitted for active turn");
}

TEST(AiGatewaySessionHandlerTest, RejectsTurnCompletedForUnknownTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const absl::StatusOr<EmitResult> result = handler.EmitTurnCompleted("turn_missing");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().message(), "turn_id does not match the active turn");
}

TEST(AiGatewaySessionHandlerTest, EmitErrorOmitsIdsBeforeSessionStart) {
    GatewaySessionHandler handler("srv_test");

    const absl::StatusOr<EmitResult> result =
        handler.EmitError(std::nullopt, "bad_request", "invalid frame");

    ASSERT_TRUE(result.ok()) << result.status().ToString();
    ASSERT_EQ(result->outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result->outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_FALSE(error.session_id.has_value());
    EXPECT_FALSE(error.turn_id.has_value());
}

TEST(AiGatewaySessionHandlerTest, EmitErrorIncludesSessionAndTurnAfterSessionStart) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler
                    .HandleIncomingJson(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const absl::StatusOr<EmitResult> result =
        handler.EmitError("turn_1", "bad_request", "missing text");

    ASSERT_TRUE(result.ok()) << result.status().ToString();
    ASSERT_EQ(result->outgoing_frames.size(), 1U);
    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(result->outgoing_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    ASSERT_TRUE(error.session_id.has_value());
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
    EXPECT_EQ(*error.turn_id, "turn_1");
}

} // namespace
} // namespace isla::server::ai_gateway
