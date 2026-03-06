#include "isla/shared/ai_gateway_protocol.hpp"
#include "isla/shared/ai_gateway_session.hpp"

#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace isla::shared::ai_gateway {
namespace {

TEST(AiGatewayProtocolTest, MessageTypeNamesRemainStable) {
    EXPECT_STREQ(message_type_name(MessageType::SessionStart), "session.start");
    EXPECT_STREQ(message_type_name(MessageType::AudioOutput), "audio.output");
    EXPECT_STREQ(message_type_name(MessageType::TurnCancelled), "turn.cancelled");
    EXPECT_STREQ(message_type_name(MessageType::Error), "error");
}

TEST(AiGatewayProtocolTest, ParsesAudioOutputMessage) {
    const MessageParseResult result = parse_json_message(R"json(
        {
          "type": "audio.output",
          "turn_id": "turn_1",
          "mime_type": "audio/wav",
          "audio_base64": "UklGRg=="
        }
    )json");

    ASSERT_TRUE(result.ok) << result.error_message;
    ASSERT_TRUE(result.message.has_value());
    ASSERT_TRUE(std::holds_alternative<AudioOutputMessage>(*result.message));

    const auto& parsed = std::get<AudioOutputMessage>(*result.message);
    EXPECT_EQ(parsed.turn_id, "turn_1");
    EXPECT_EQ(parsed.mime_type, "audio/wav");
    EXPECT_EQ(parsed.audio_base64, "UklGRg==");
}

TEST(AiGatewayProtocolTest, RoundTripsSessionStartWithClientSessionId) {
    const GatewayMessage original = SessionStartMessage{"client_123"};

    const MessageParseResult parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok) << parsed.error_message;
    ASSERT_TRUE(parsed.message.has_value());
    ASSERT_TRUE(std::holds_alternative<SessionStartMessage>(*parsed.message));

    const auto& round_trip = std::get<SessionStartMessage>(*parsed.message);
    ASSERT_TRUE(round_trip.client_session_id.has_value());
    EXPECT_EQ(*round_trip.client_session_id, "client_123");
}

TEST(AiGatewayProtocolTest, RoundTripsSessionLifecycleMessages) {
    {
        const GatewayMessage original = SessionStartedMessage{"srv_123"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<SessionStartedMessage>(*parsed.message));
        EXPECT_EQ(std::get<SessionStartedMessage>(*parsed.message).session_id, "srv_123");
    }

    {
        const GatewayMessage original = SessionEndMessage{"srv_123"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<SessionEndMessage>(*parsed.message));
        EXPECT_EQ(std::get<SessionEndMessage>(*parsed.message).session_id, "srv_123");
    }

    {
        const GatewayMessage original = SessionEndedMessage{"srv_123"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<SessionEndedMessage>(*parsed.message));
        EXPECT_EQ(std::get<SessionEndedMessage>(*parsed.message).session_id, "srv_123");
    }
}

TEST(AiGatewayProtocolTest, RoundTripsTurnMessages) {
    {
        const GatewayMessage original = TextInputMessage{"turn_1", "hello"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<TextInputMessage>(*parsed.message));
        const auto& message = std::get<TextInputMessage>(*parsed.message);
        EXPECT_EQ(message.turn_id, "turn_1");
        EXPECT_EQ(message.text, "hello");
    }

    {
        const GatewayMessage original = TurnCompletedMessage{"turn_1"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<TurnCompletedMessage>(*parsed.message));
        EXPECT_EQ(std::get<TurnCompletedMessage>(*parsed.message).turn_id, "turn_1");
    }

    {
        const GatewayMessage original = TurnCancelMessage{"turn_1"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<TurnCancelMessage>(*parsed.message));
        EXPECT_EQ(std::get<TurnCancelMessage>(*parsed.message).turn_id, "turn_1");
    }

    {
        const GatewayMessage original = TurnCancelledMessage{"turn_1"};
        const MessageParseResult parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok) << parsed.error_message;
        ASSERT_TRUE(std::holds_alternative<TurnCancelledMessage>(*parsed.message));
        EXPECT_EQ(std::get<TurnCancelledMessage>(*parsed.message).turn_id, "turn_1");
    }
}

TEST(AiGatewayProtocolTest, RoundTripsTextOutputMessage) {
    const GatewayMessage original = TextOutputMessage{"turn_42", "hello from gateway"};

    const MessageParseResult parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok) << parsed.error_message;
    ASSERT_TRUE(parsed.message.has_value());
    ASSERT_TRUE(std::holds_alternative<TextOutputMessage>(*parsed.message));

    const auto& round_trip = std::get<TextOutputMessage>(*parsed.message);
    EXPECT_EQ(round_trip.turn_id, "turn_42");
    EXPECT_EQ(round_trip.text, "hello from gateway");
}

TEST(AiGatewayProtocolTest, RoundTripsErrorWithIds) {
    const GatewayMessage original =
        ErrorMessage{std::string("srv_123"), std::string("turn_1"), "bad_request", "Missing text"};

    const MessageParseResult parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok) << parsed.error_message;
    ASSERT_TRUE(parsed.message.has_value());
    ASSERT_TRUE(std::holds_alternative<ErrorMessage>(*parsed.message));

    const auto& round_trip = std::get<ErrorMessage>(*parsed.message);
    ASSERT_TRUE(round_trip.session_id.has_value());
    ASSERT_TRUE(round_trip.turn_id.has_value());
    EXPECT_EQ(*round_trip.session_id, "srv_123");
    EXPECT_EQ(*round_trip.turn_id, "turn_1");
    EXPECT_EQ(round_trip.code, "bad_request");
    EXPECT_EQ(round_trip.message, "Missing text");
}

TEST(AiGatewayProtocolTest, ParsesErrorWithoutTurnId) {
    const MessageParseResult result = parse_json_message(R"json(
        {
          "type": "error",
          "session_id": "srv_123",
          "code": "bad_request",
          "message": "Missing text"
        }
    )json");

    ASSERT_TRUE(result.ok) << result.error_message;
    ASSERT_TRUE(result.message.has_value());
    ASSERT_TRUE(std::holds_alternative<ErrorMessage>(*result.message));

    const auto& parsed = std::get<ErrorMessage>(*result.message);
    ASSERT_TRUE(parsed.session_id.has_value());
    EXPECT_EQ(*parsed.session_id, "srv_123");
    EXPECT_FALSE(parsed.turn_id.has_value());
    EXPECT_EQ(parsed.code, "bad_request");
    EXPECT_EQ(parsed.message, "Missing text");
}

TEST(AiGatewayProtocolTest, RejectsUnknownType) {
    const MessageParseResult result =
        parse_json_message(R"json({"type":"unknown.message","turn_id":"turn_1"})json");

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error_message.find("unknown type"), std::string::npos);
}

TEST(AiGatewayProtocolTest, RejectsAudioOutputWithoutPayload) {
    const MessageParseResult result = parse_json_message(R"json(
        {
          "type": "audio.output",
          "turn_id": "turn_1",
          "mime_type": "audio/wav"
        }
    )json");

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error_message.find("audio.output"), std::string::npos);
}

TEST(AiGatewayProtocolTest, RejectsWrongFieldTypes) {
    {
        const MessageParseResult result = parse_json_message(R"json(
            {
              "type": "text.input",
              "turn_id": 7,
              "text": "hello"
            }
        )json");
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error_message.find("text.input"), std::string::npos);
    }

    {
        const MessageParseResult result = parse_json_message(R"json(
            {
              "type": "session.end",
              "session_id": 123
            }
        )json");
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error_message.find("session.end"), std::string::npos);
    }

    {
        const MessageParseResult result = parse_json_message(R"json(
            {
              "type": "error",
              "code": false,
              "message": "bad"
            }
        )json");
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error_message.find("error"), std::string::npos);
    }
}

TEST(AiGatewaySessionTest, RequiresSessionBeforeTurn) {
    SessionState state;
    std::string error_message;

    EXPECT_FALSE(state.begin_turn("turn_1", &error_message));
    EXPECT_EQ(error_message, "session is not active");
}

TEST(AiGatewaySessionTest, EnforcesSingleInFlightTurn) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123"));
    ASSERT_TRUE(state.begin_turn("turn_1"));

    std::string error_message;
    EXPECT_FALSE(state.begin_turn("turn_2", &error_message));
    EXPECT_EQ(error_message, "only one turn may be in flight per session");
}

TEST(AiGatewaySessionTest, EnforcesTextBeforeAudio) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123"));
    ASSERT_TRUE(state.begin_turn("turn_1"));

    std::string error_message;
    EXPECT_FALSE(state.mark_audio_output("turn_1", &error_message));
    EXPECT_EQ(error_message, "audio output requires text output first");

    ASSERT_TRUE(state.mark_text_output("turn_1"));
    ASSERT_TRUE(state.mark_audio_output("turn_1"));
}

TEST(AiGatewaySessionTest, TracksCancellationLifecycle) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123"));
    ASSERT_TRUE(state.begin_turn("turn_1"));
    ASSERT_TRUE(state.request_turn_cancel("turn_1"));
    ASSERT_TRUE(state.snapshot().active_turn.has_value());
    EXPECT_EQ(state.snapshot().active_turn->status, TurnStatus::CancelRequested);

    ASSERT_TRUE(state.confirm_turn_cancel("turn_1"));
    EXPECT_FALSE(state.snapshot().active_turn.has_value());
}

TEST(AiGatewaySessionTest, RejectsEndingSessionWithActiveTurn) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123"));
    ASSERT_TRUE(state.begin_turn("turn_1"));

    std::string error_message;
    EXPECT_FALSE(state.end(&error_message));
    EXPECT_EQ(error_message, "session cannot end while a turn is in flight");
}

TEST(AiGatewaySessionTest, EndsSessionAfterTurnCompletes) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123"));
    ASSERT_TRUE(state.begin_turn("turn_1"));
    ASSERT_TRUE(state.mark_text_output("turn_1"));
    ASSERT_TRUE(state.complete_turn("turn_1"));
    ASSERT_TRUE(state.end());

    EXPECT_EQ(state.snapshot().status, SessionStatus::Ended);
    EXPECT_EQ(state.snapshot().session_id, "srv_123");
    EXPECT_FALSE(state.snapshot().active_turn.has_value());
}

} // namespace
} // namespace isla::shared::ai_gateway
