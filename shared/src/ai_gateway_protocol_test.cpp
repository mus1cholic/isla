#include "isla/shared/ai_gateway_protocol.hpp"
#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace isla::shared::ai_gateway {
namespace {

TEST(AiGatewayProtocolTest, MessageTypeNamesRemainStable) {
    EXPECT_STREQ(message_type_name(MessageType::SessionStart), "session.start");
    EXPECT_STREQ(message_type_name(MessageType::TranscriptSeed), "transcript.seed");
    EXPECT_STREQ(message_type_name(MessageType::TranscriptSeeded), "transcript.seeded");
    EXPECT_STREQ(message_type_name(MessageType::AudioOutput), "audio.output");
    EXPECT_STREQ(message_type_name(MessageType::TurnCancelled), "turn.cancelled");
    EXPECT_STREQ(message_type_name(MessageType::Error), "error");
}

TEST(AiGatewayProtocolTest, ParsesAudioOutputMessage) {
    const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
        {
          "type": "audio.output",
          "turn_id": "turn_1",
          "mime_type": "audio/wav",
          "audio_base64": "UklGRg=="
        }
    )json");

    ASSERT_TRUE(result.ok()) << result.status().ToString();
    ASSERT_TRUE(std::holds_alternative<AudioOutputMessage>(*result));

    const auto& parsed = std::get<AudioOutputMessage>(*result);
    EXPECT_EQ(parsed.turn_id, "turn_1");
    EXPECT_EQ(parsed.mime_type, "audio/wav");
    EXPECT_EQ(parsed.audio_base64, "UklGRg==");
}

TEST(AiGatewayProtocolTest, RoundTripsSessionStartWithClientSessionId) {
    const GatewayMessage original = SessionStartMessage{
        .client_session_id = "client_123",
        .session_start_time = "2026-03-14T09:59:00Z",
        .evaluation_reference_time = "2026-03-20T08:00:00Z",
    };

    const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<SessionStartMessage>(*parsed));

    const auto& round_trip = std::get<SessionStartMessage>(*parsed);
    ASSERT_TRUE(round_trip.client_session_id.has_value());
    EXPECT_EQ(*round_trip.client_session_id, "client_123");
    ASSERT_TRUE(round_trip.session_start_time.has_value());
    EXPECT_EQ(*round_trip.session_start_time, "2026-03-14T09:59:00Z");
    ASSERT_TRUE(round_trip.evaluation_reference_time.has_value());
    EXPECT_EQ(*round_trip.evaluation_reference_time, "2026-03-20T08:00:00Z");
}

TEST(AiGatewayProtocolTest, RoundTripsSessionLifecycleMessages) {
    {
        const GatewayMessage original = SessionStartedMessage{ "srv_123" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<SessionStartedMessage>(*parsed));
        EXPECT_EQ(std::get<SessionStartedMessage>(*parsed).session_id, "srv_123");
    }

    {
        const GatewayMessage original = SessionEndMessage{ "srv_123" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<SessionEndMessage>(*parsed));
        EXPECT_EQ(std::get<SessionEndMessage>(*parsed).session_id, "srv_123");
    }

    {
        const GatewayMessage original = SessionEndedMessage{ "srv_123" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<SessionEndedMessage>(*parsed));
        EXPECT_EQ(std::get<SessionEndedMessage>(*parsed).session_id, "srv_123");
    }
}

TEST(AiGatewayProtocolTest, RoundTripsTurnMessages) {
    {
        const GatewayMessage original =
            TranscriptSeedMessage{ .turn_id = "turn_1",
                                   .role = "assistant",
                                   .text = "seeded",
                                   .create_time = "2026-03-14T10:00:05Z" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TranscriptSeedMessage>(*parsed));
        const auto& message = std::get<TranscriptSeedMessage>(*parsed);
        EXPECT_EQ(message.turn_id, "turn_1");
        EXPECT_EQ(message.role, "assistant");
        EXPECT_EQ(message.text, "seeded");
        ASSERT_TRUE(message.create_time.has_value());
        EXPECT_EQ(*message.create_time, "2026-03-14T10:00:05Z");
    }

    {
        const GatewayMessage original =
            TranscriptSeededMessage{ .turn_id = "turn_1", .role = "assistant" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TranscriptSeededMessage>(*parsed));
        const auto& message = std::get<TranscriptSeededMessage>(*parsed);
        EXPECT_EQ(message.turn_id, "turn_1");
        EXPECT_EQ(message.role, "assistant");
    }

    {
        const GatewayMessage original = TextInputMessage{
            .turn_id = "turn_1",
            .text = "hello",
            .create_time = "2026-03-15T11:30:00Z",
        };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TextInputMessage>(*parsed));
        const auto& message = std::get<TextInputMessage>(*parsed);
        EXPECT_EQ(message.turn_id, "turn_1");
        EXPECT_EQ(message.text, "hello");
        ASSERT_TRUE(message.create_time.has_value());
        EXPECT_EQ(*message.create_time, "2026-03-15T11:30:00Z");
    }

    {
        const GatewayMessage original = TurnCompletedMessage{ "turn_1" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TurnCompletedMessage>(*parsed));
        EXPECT_EQ(std::get<TurnCompletedMessage>(*parsed).turn_id, "turn_1");
    }

    {
        const GatewayMessage original = TurnCancelMessage{ "turn_1" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TurnCancelMessage>(*parsed));
        EXPECT_EQ(std::get<TurnCancelMessage>(*parsed).turn_id, "turn_1");
    }

    {
        const GatewayMessage original = TurnCancelledMessage{ "turn_1" };
        const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));
        ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
        ASSERT_TRUE(std::holds_alternative<TurnCancelledMessage>(*parsed));
        EXPECT_EQ(std::get<TurnCancelledMessage>(*parsed).turn_id, "turn_1");
    }
}

TEST(AiGatewayProtocolTest, RoundTripsTextOutputMessage) {
    const GatewayMessage original =
        TextOutputMessage{ .turn_id = "turn_42", .text = "hello from gateway" };

    const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<TextOutputMessage>(*parsed));

    const auto& round_trip = std::get<TextOutputMessage>(*parsed);
    EXPECT_EQ(round_trip.turn_id, "turn_42");
    EXPECT_EQ(round_trip.text, "hello from gateway");
}

TEST(AiGatewayProtocolTest, RoundTripsErrorWithIds) {
    const GatewayMessage original = ErrorMessage{ .session_id = std::string("srv_123"),
                                                  .turn_id = std::string("turn_1"),
                                                  .code = "bad_request",
                                                  .message = "Missing text" };

    const absl::StatusOr<GatewayMessage> parsed = parse_json_message(to_json_string(original));

    ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<ErrorMessage>(*parsed));

    const auto& round_trip = std::get<ErrorMessage>(*parsed);
    ASSERT_TRUE(round_trip.session_id.has_value());
    ASSERT_TRUE(round_trip.turn_id.has_value());
    EXPECT_EQ(*round_trip.session_id, "srv_123");
    EXPECT_EQ(*round_trip.turn_id, "turn_1");
    EXPECT_EQ(round_trip.code, "bad_request");
    EXPECT_EQ(round_trip.message, "Missing text");
}

TEST(AiGatewayProtocolTest, ParsesErrorWithoutTurnId) {
    const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
        {
          "type": "error",
          "session_id": "srv_123",
          "code": "bad_request",
          "message": "Missing text"
        }
    )json");

    ASSERT_TRUE(result.ok()) << result.status().ToString();
    ASSERT_TRUE(std::holds_alternative<ErrorMessage>(*result));

    const auto& parsed = std::get<ErrorMessage>(*result);
    ASSERT_TRUE(parsed.session_id.has_value());
    EXPECT_EQ(*parsed.session_id, "srv_123");
    EXPECT_FALSE(parsed.turn_id.has_value());
    EXPECT_EQ(parsed.code, "bad_request");
    EXPECT_EQ(parsed.message, "Missing text");
}

TEST(AiGatewayProtocolTest, RejectsUnknownType) {
    const absl::StatusOr<GatewayMessage> result =
        parse_json_message(R"json({"type":"unknown.message","turn_id":"turn_1"})json");

    EXPECT_FALSE(result.ok());
    EXPECT_NE(std::string(result.status().message()).find("unknown type"), std::string::npos);
}

TEST(AiGatewayProtocolTest, RejectsAudioOutputWithoutPayload) {
    const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
        {
          "type": "audio.output",
          "turn_id": "turn_1",
          "mime_type": "audio/wav"
        }
    )json");

    EXPECT_FALSE(result.ok());
    EXPECT_NE(std::string(result.status().message()).find("audio.output"), std::string::npos);
}

TEST(AiGatewayProtocolTest, RejectsWrongFieldTypes) {
    {
        const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
            {
              "type": "text.input",
              "turn_id": 7,
              "text": "hello"
            }
        )json");
        EXPECT_FALSE(result.ok());
        EXPECT_NE(std::string(result.status().message()).find("text.input"), std::string::npos);
    }

    {
        const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
            {
              "type": "session.end",
              "session_id": 123
            }
        )json");
        EXPECT_FALSE(result.ok());
        EXPECT_NE(std::string(result.status().message()).find("session.end"), std::string::npos);
    }

    {
        const absl::StatusOr<GatewayMessage> result = parse_json_message(R"json(
            {
              "type": "error",
              "code": false,
              "message": "bad"
            }
        )json");
        EXPECT_FALSE(result.ok());
        EXPECT_NE(std::string(result.status().message()).find("error"), std::string::npos);
    }
}

} // namespace
} // namespace isla::shared::ai_gateway
