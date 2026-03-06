#include "ai_gateway_session_handler.hpp"

#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace protocol = isla::shared::ai_gateway;

protocol::GatewayMessage parse_frame_or_die(const std::string& frame) {
    const protocol::MessageParseResult parsed = protocol::parse_json_message(frame);
    EXPECT_TRUE(parsed.ok) << parsed.error_message;
    EXPECT_TRUE(parsed.message.has_value());
    return *parsed.message;
}

TEST(AiGatewaySessionHandlerTest, SessionStartEmitsSessionStarted) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.start"})json");

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    ASSERT_FALSE(result.accepted_turn.has_value());

    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(frame));
    EXPECT_EQ(std::get<protocol::SessionStartedMessage>(frame).session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateSessionStart) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.start"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    ASSERT_TRUE(error.session_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, RejectsTextInputBeforeSessionStart) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);

    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
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
}

TEST(AiGatewaySessionHandlerTest, RejectsConcurrentSecondTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult result = handler.HandleIncomingJson(
        R"json({"type":"text.input","turn_id":"turn_2","text":"again"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_2");
}

TEST(AiGatewaySessionHandlerTest, EmitsTextAudioAndCompletionForAcceptedTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const EmitResult text = handler.EmitTextOutput("turn_1", "hi");
    const EmitResult audio = handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");
    const EmitResult completed = handler.EmitTurnCompleted("turn_1");

    ASSERT_TRUE(text.ok);
    ASSERT_TRUE(audio.ok);
    ASSERT_TRUE(completed.ok);
    ASSERT_EQ(text.outgoing_frames.size(), 1U);
    ASSERT_EQ(audio.outgoing_frames.size(), 1U);
    ASSERT_EQ(completed.outgoing_frames.size(), 1U);
    EXPECT_FALSE(handler.snapshot().active_turn.has_value());

    EXPECT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(
        parse_frame_or_die(text.outgoing_frames.front())));
    EXPECT_TRUE(std::holds_alternative<protocol::AudioOutputMessage>(
        parse_frame_or_die(audio.outgoing_frames.front())));
    EXPECT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(
        parse_frame_or_die(completed.outgoing_frames.front())));
}

TEST(AiGatewaySessionHandlerTest, RejectsAudioBeforeText) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const EmitResult result = handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_message, "audio output requires text output first");
}

TEST(AiGatewaySessionHandlerTest, SurfacesTurnCancelAsApplicationEvent) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult cancel =
        handler.HandleIncomingJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json");
    ASSERT_TRUE(cancel.ok);
    ASSERT_TRUE(cancel.cancel_requested.has_value());
    EXPECT_EQ(cancel.cancel_requested->session_id, "srv_test");
    EXPECT_EQ(cancel.cancel_requested->turn_id, "turn_1");

    const EmitResult cancelled = handler.EmitTurnCancelled("turn_1");
    ASSERT_TRUE(cancelled.ok);
    EXPECT_FALSE(handler.snapshot().active_turn.has_value());
    EXPECT_TRUE(std::holds_alternative<protocol::TurnCancelledMessage>(
        parse_frame_or_die(cancelled.outgoing_frames.front())));
}

TEST(AiGatewaySessionHandlerTest, RejectsServerOwnedInboundMessages) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"text.output","turn_id":"turn_1","text":"x"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<protocol::ErrorMessage>(
        parse_frame_or_die(result.outgoing_frames.front())));
}

TEST(AiGatewaySessionHandlerTest, RejectsSessionEndWhileTurnIsActive) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_test"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<protocol::ErrorMessage>(
        parse_frame_or_die(result.outgoing_frames.front())));
}

TEST(AiGatewaySessionHandlerTest, RejectsSessionEndWithMismatchedSessionId) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_other"})json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    ASSERT_TRUE(error.session_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
}

TEST(AiGatewaySessionHandlerTest, SessionEndEmitsSessionEndedAfterTurnCompletes) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok);
    ASSERT_TRUE(handler.EmitTurnCompleted("turn_1").ok);

    const HandleIncomingResult result =
        handler.HandleIncomingJson(R"json({"type":"session.end","session_id":"srv_test"})json");

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.should_close);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    EXPECT_EQ(handler.snapshot().status, protocol::SessionStatus::Ended);
    EXPECT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(
        parse_frame_or_die(result.outgoing_frames.front())));
}

TEST(AiGatewaySessionHandlerTest, ParseFailureReturnsErrorFrame) {
    GatewaySessionHandler handler("srv_test");

    const HandleIncomingResult result = handler.HandleIncomingJson("{not valid json");

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    EXPECT_FALSE(error.session_id.has_value());
    EXPECT_EQ(error.code, "bad_request");
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateTextOutput) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok);

    const EmitResult result = handler.EmitTextOutput("turn_1", "hi again");

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_message, "text output already emitted for active turn");
}

TEST(AiGatewaySessionHandlerTest, RejectsDuplicateAudioOutput) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);
    ASSERT_TRUE(handler.EmitTextOutput("turn_1", "hi").ok);
    ASSERT_TRUE(handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==").ok);

    const EmitResult result = handler.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_message, "audio output already emitted for active turn");
}

TEST(AiGatewaySessionHandlerTest, RejectsTurnCompletedForUnknownTurn) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);

    const EmitResult result = handler.EmitTurnCompleted("turn_missing");

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_message, "turn_id does not match the active turn");
}

TEST(AiGatewaySessionHandlerTest, EmitErrorOmitsIdsBeforeSessionStart) {
    GatewaySessionHandler handler("srv_test");

    const EmitResult result = handler.EmitError(std::nullopt, "bad_request", "invalid frame");

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    EXPECT_FALSE(error.session_id.has_value());
    EXPECT_FALSE(error.turn_id.has_value());
}

TEST(AiGatewaySessionHandlerTest, EmitErrorIncludesSessionAndTurnAfterSessionStart) {
    GatewaySessionHandler handler("srv_test");
    ASSERT_TRUE(handler.HandleIncomingJson(R"json({"type":"session.start"})json").ok);
    ASSERT_TRUE(handler.HandleIncomingJson(
                           R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok);

    const EmitResult result = handler.EmitError("turn_1", "bad_request", "missing text");

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.outgoing_frames.size(), 1U);
    const protocol::GatewayMessage frame = parse_frame_or_die(result.outgoing_frames.front());
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(frame));
    const auto& error = std::get<protocol::ErrorMessage>(frame);
    ASSERT_TRUE(error.session_id.has_value());
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.session_id, "srv_test");
    EXPECT_EQ(*error.turn_id, "turn_1");
}

} // namespace
} // namespace isla::server::ai_gateway
