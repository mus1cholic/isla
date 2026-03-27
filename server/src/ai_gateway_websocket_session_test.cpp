#include "isla/server/ai_gateway_websocket_session.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "ai_gateway_test_mocks.hpp"
#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace protocol = isla::shared::ai_gateway;
constexpr std::string_view kSessionStartJson =
    R"json({"type":"session.start","user_id":"websocket_test_user"})json";
using ::testing::_;
using ::testing::NiceMock;

absl::StatusOr<protocol::GatewayMessage> parse_frame(const std::string& frame) {
    return protocol::parse_json_message(frame);
}

class RecordingWebSocketConnection final : public NiceMock<test::MockGatewayWebSocketConnection> {
  public:
    RecordingWebSocketConnection() {
        ON_CALL(*this, SendTextFrame(_)).WillByDefault([this](std::string_view frame) {
            if (fail_next_send_) {
                fail_next_send_ = false;
                return absl::UnavailableError("send failed");
            }
            sent_frames.emplace_back(frame);
            return absl::OkStatus();
        });
        ON_CALL(*this, Close(_)).WillByDefault([this](GatewayTransportCloseMode mode) {
            ++close_calls;
            close_modes.push_back(mode);
        });
    }

    bool fail_next_send_ = false;
    int close_calls = 0;
    std::vector<GatewayTransportCloseMode> close_modes;
    std::vector<std::string> sent_frames;
};

class RecordingEventSink final : public NiceMock<test::MockGatewaySessionEventSink> {
  public:
    RecordingEventSink() {
        ON_CALL(*this, OnSessionStarted(_)).WillByDefault([this](const SessionStartedEvent& event) {
            started_sessions.push_back(event);
        });
        ON_CALL(*this, HandleTranscriptSeed(_))
            .WillByDefault([this](const TranscriptSeedEvent& event) {
                transcript_seeds.push_back(event);
                return absl::OkStatus();
            });
        ON_CALL(*this, OnTurnAccepted(_)).WillByDefault([this](const TurnAcceptedEvent& event) {
            accepted_turns.push_back(event);
        });
        ON_CALL(*this, OnTurnCancelRequested(_))
            .WillByDefault([this](const TurnCancelRequestedEvent& event) {
                cancel_requests.push_back(event);
            });
        ON_CALL(*this, OnSessionClosed(_)).WillByDefault([this](const SessionClosedEvent& event) {
            closed_sessions.push_back(event);
        });
    }

    std::vector<SessionStartedEvent> started_sessions;
    std::vector<TranscriptSeedEvent> transcript_seeds;
    std::vector<TurnAcceptedEvent> accepted_turns;
    std::vector<TurnCancelRequestedEvent> cancel_requests;
    std::vector<SessionClosedEvent> closed_sessions;
};

class RecordingTelemetrySink final : public NiceMock<test::MockTelemetrySink> {
  public:
    RecordingTelemetrySink() {
        ON_CALL(*this, OnTurnAccepted(_))
            .WillByDefault([this](const TurnTelemetryContext& context) {
                accepted_turns.push_back(
                    { .session_id = context.session_id, .turn_id = context.turn_id });
            });
    }

    mutable std::vector<TurnAcceptedEvent> accepted_turns;
};

TEST(AiGatewayWebSocketSessionTest, UuidSessionIdGeneratorCreatesValidAndUniqueUUIDs) {
    UuidSessionIdGenerator generator;

    const std::string id1 = generator.NextSessionId();
    const std::string id2 = generator.NextSessionId();

    EXPECT_NE(id1, id2);

    boost::uuids::string_generator sgen;
    boost::uuids::uuid u1;
    boost::uuids::uuid u2;
    ASSERT_NO_THROW(u1 = sgen(id1));
    ASSERT_NO_THROW(u2 = sgen(id2));

    EXPECT_EQ(u1.version(), boost::uuids::uuid::version_random_number_based);
    EXPECT_EQ(u2.version(), boost::uuids::uuid::version_random_number_based);
}

TEST(AiGatewayWebSocketSessionTest, UuidSessionIdGeneratorProducesUniqueIdsAcrossThreads) {
    UuidSessionIdGenerator generator;
    constexpr std::size_t kNumThreads = 10;
    constexpr std::size_t kIdsPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    std::mutex results_mutex;
    std::vector<std::string> all_ids;
    all_ids.reserve(kNumThreads * kIdsPerThread);

    for (std::size_t i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&]() {
            std::vector<std::string> local_ids;
            local_ids.reserve(kIdsPerThread);
            for (std::size_t j = 0; j < kIdsPerThread; ++j) {
                local_ids.push_back(generator.NextSessionId());
            }

            std::lock_guard<std::mutex> lock(results_mutex);
            all_ids.insert(all_ids.end(), local_ids.begin(), local_ids.end());
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::unordered_set<std::string> unique_ids(all_ids.begin(), all_ids.end());
    EXPECT_EQ(unique_ids.size(), kNumThreads * kIdsPerThread);
}

TEST(AiGatewayWebSocketSessionTest, SequentialSessionIdGeneratorCreatesOrderedIds) {
    SequentialSessionIdGenerator generator("srv_test_");

    EXPECT_EQ(generator.NextSessionId(), "srv_test_1");
    EXPECT_EQ(generator.NextSessionId(), "srv_test_2");
    EXPECT_EQ(generator.NextSessionId(), "srv_test_3");
}

TEST(AiGatewayWebSocketSessionTest, FactoryGeneratesPerConnectionSessionIds) {
    GatewayWebSocketSessionFactory factory(
        std::make_unique<SequentialSessionIdGenerator>("srv_test_"));
    RecordingWebSocketConnection first_connection;
    RecordingWebSocketConnection second_connection;

    const std::unique_ptr<GatewayWebSocketSessionAdapter> first =
        factory.CreateSession(first_connection);
    const std::unique_ptr<GatewayWebSocketSessionAdapter> second =
        factory.CreateSession(second_connection);

    EXPECT_EQ(first->session_id(), "srv_test_1");
    EXPECT_EQ(second->session_id(), "srv_test_2");
}

TEST(AiGatewayWebSocketSessionTest, SessionStartWritesSessionStartedFrame) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.started_sessions.size(), 1U);
    EXPECT_EQ(sink.started_sessions.front().session_id, "srv_test");

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(connection.sent_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*frame));
    EXPECT_EQ(std::get<protocol::SessionStartedMessage>(*frame).session_id, "srv_test");
}

TEST(AiGatewayWebSocketSessionTest, AcceptedTurnIsForwardedToEventSink) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_EQ(sink.accepted_turns.size(), 1U);
    EXPECT_EQ(sink.accepted_turns.front().session_id, "srv_test");
    EXPECT_EQ(sink.accepted_turns.front().turn_id, "turn_1");
    EXPECT_EQ(sink.accepted_turns.front().text, "hello");
}

TEST(AiGatewayWebSocketSessionTest, AcceptedTurnCreatesTelemetryContextAtGatewayBoundary) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink, telemetry_sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_EQ(sink.accepted_turns.size(), 1U);
    ASSERT_NE(sink.accepted_turns.front().telemetry_context, nullptr);
    EXPECT_EQ(sink.accepted_turns.front().telemetry_context->session_id, "srv_test");
    EXPECT_EQ(sink.accepted_turns.front().telemetry_context->turn_id, "turn_1");
    EXPECT_EQ(sink.accepted_turns.front().telemetry_context->sink, telemetry_sink);

    ASSERT_EQ(telemetry_sink->accepted_turns.size(), 1U);
    EXPECT_EQ(telemetry_sink->accepted_turns.front().session_id, "srv_test");
    EXPECT_EQ(telemetry_sink->accepted_turns.front().turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, TranscriptSeedIsForwardedAndAcknowledged) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(
        session
            .HandleIncomingTextFrame(
                R"json({"type":"transcript.seed","turn_id":"turn_seed","role":"assistant","text":"seeded context"})json")
            .ok());

    ASSERT_EQ(sink.transcript_seeds.size(), 1U);
    EXPECT_EQ(sink.transcript_seeds.front().session_id, "srv_test");
    EXPECT_EQ(sink.transcript_seeds.front().turn_id, "turn_seed");
    EXPECT_EQ(sink.transcript_seeds.front().role, "assistant");
    EXPECT_EQ(sink.transcript_seeds.front().text, "seeded context");

    ASSERT_EQ(connection.sent_frames.size(), 2U);
    const absl::StatusOr<protocol::GatewayMessage> frame = parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TranscriptSeededMessage>(*frame));
    EXPECT_EQ(std::get<protocol::TranscriptSeededMessage>(*frame).turn_id, "turn_seed");
    EXPECT_EQ(std::get<protocol::TranscriptSeededMessage>(*frame).role, "assistant");
}

TEST(AiGatewayWebSocketSessionTest, TranscriptSeedFailureReturnsErrorFrameAndKeepsSessionOpen) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ON_CALL(sink, HandleTranscriptSeed(_)).WillByDefault([](const TranscriptSeedEvent& /*event*/) {
        return absl::InvalidArgumentError("seed rejected");
    });

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    const absl::Status status = session.HandleIncomingTextFrame(
        R"json({"type":"transcript.seed","turn_id":"turn_seed","role":"assistant","text":"seeded context"})json");

    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 2U);
    const absl::StatusOr<protocol::GatewayMessage> frame = parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_EQ(error.code, "bad_request");
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_seed");
    EXPECT_EQ(error.message, "seed rejected");
}

TEST(AiGatewayWebSocketSessionTest, RejectedClientFrameSendsErrorAndKeepsSessionOpen) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    const absl::Status status = session.HandleIncomingTextFrame(
        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json");

    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(connection.sent_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*frame));
    const auto& error = std::get<protocol::ErrorMessage>(*frame);
    EXPECT_EQ(error.code, "bad_request");
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_1");

    EXPECT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
}

TEST(AiGatewayWebSocketSessionTest, TurnCancelIsForwardedToEventSink) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"turn.cancel","turn_id":"turn_1"})json")
            .ok());

    ASSERT_EQ(sink.cancel_requests.size(), 1U);
    EXPECT_EQ(sink.cancel_requests.front().session_id, "srv_test");
    EXPECT_EQ(sink.cancel_requests.front().turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, ServerOwnedEmitTextOutputAndCompletionPreserveFrameOrder) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_TRUE(session.EmitTextOutput("turn_1", "stub reply").ok());
    ASSERT_TRUE(session.EmitTurnCompleted("turn_1").ok());

    ASSERT_EQ(connection.sent_frames.size(), 3U);

    const absl::StatusOr<protocol::GatewayMessage> text_frame =
        parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(text_frame.ok()) << text_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*text_frame));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_frame).turn_id, "turn_1");
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_frame).text, "stub reply");

    const absl::StatusOr<protocol::GatewayMessage> completed_frame =
        parse_frame(connection.sent_frames[2]);
    ASSERT_TRUE(completed_frame.ok()) << completed_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*completed_frame));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*completed_frame).turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, EmitTextOutputRejectsWithoutActiveTurn) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());

    const absl::Status status = session.EmitTextOutput("turn_1", "stub reply");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "turn_id does not match the active turn");
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());
}

TEST(AiGatewayWebSocketSessionTest, EmitTextOutputSendFailureClosesSession) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.EmitTextOutput("turn_1", "stub reply");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, EmitAudioOutputSendsFrameAfterTextOutput) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(session.EmitTextOutput("turn_1", "stub reply").ok());

    ASSERT_TRUE(session.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==").ok());

    ASSERT_EQ(connection.sent_frames.size(), 3U);
    const absl::StatusOr<protocol::GatewayMessage> audio_frame =
        parse_frame(connection.sent_frames[2]);
    ASSERT_TRUE(audio_frame.ok()) << audio_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::AudioOutputMessage>(*audio_frame));
    EXPECT_EQ(std::get<protocol::AudioOutputMessage>(*audio_frame).turn_id, "turn_1");
    EXPECT_EQ(std::get<protocol::AudioOutputMessage>(*audio_frame).mime_type, "audio/wav");
    EXPECT_EQ(std::get<protocol::AudioOutputMessage>(*audio_frame).audio_base64, "UklGRg==");
}

TEST(AiGatewayWebSocketSessionTest, EmitAudioOutputRejectsWithoutMatchingTurnState) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    const absl::Status status = session.EmitAudioOutput("turn_2", "audio/wav", "UklGRg==");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "turn_id does not match the active turn");
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());
}

TEST(AiGatewayWebSocketSessionTest, EmitAudioOutputSendFailureClosesSession) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(session.EmitTextOutput("turn_1", "stub reply").ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.EmitAudioOutput("turn_1", "audio/wav", "UklGRg==");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 2U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, EmitTurnCancelledSendsFrameAfterCancelRequest) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"turn.cancel","turn_id":"turn_1"})json")
            .ok());

    ASSERT_TRUE(session.EmitTurnCancelled("turn_1").ok());

    ASSERT_EQ(connection.sent_frames.size(), 2U);
    const absl::StatusOr<protocol::GatewayMessage> cancelled_frame =
        parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(cancelled_frame.ok()) << cancelled_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCancelledMessage>(*cancelled_frame));
    EXPECT_EQ(std::get<protocol::TurnCancelledMessage>(*cancelled_frame).turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, EmitTurnCancelledRejectsWithoutCancelRequest) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    const absl::Status status = session.EmitTurnCancelled("turn_1");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "turn cancellation was not requested");
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());
}

TEST(AiGatewayWebSocketSessionTest, EmitTurnCancelledSendFailureClosesSession) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"turn.cancel","turn_id":"turn_1"})json")
            .ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.EmitTurnCancelled("turn_1");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, EmitErrorSendsFrameWithTurnId) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_TRUE(session.EmitError("turn_1", "transport_error", "socket failed").ok());

    ASSERT_EQ(connection.sent_frames.size(), 2U);
    const absl::StatusOr<protocol::GatewayMessage> error_frame =
        parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(error_frame.ok()) << error_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*error_frame));
    EXPECT_EQ(std::get<protocol::ErrorMessage>(*error_frame).code, "transport_error");
    ASSERT_TRUE(std::get<protocol::ErrorMessage>(*error_frame).turn_id.has_value());
    EXPECT_EQ(*std::get<protocol::ErrorMessage>(*error_frame).turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, EmitErrorRejectsEmptyCode) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());

    const absl::Status status = session.EmitError(std::nullopt, "", "socket failed");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "error emission requires non-empty code and message");
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());
}

TEST(AiGatewayWebSocketSessionTest, EmitErrorSendFailureClosesSession) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.EmitError(std::nullopt, "transport_error", "socket failed");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, SessionEndClosesTransportAfterReply) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"session.end","session_id":"srv_test"})json")
            .ok());

    ASSERT_EQ(connection.sent_frames.size(), 2U);
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Graceful);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_TRUE(session.is_closed());
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::ProtocolEnded);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "session ended");
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(connection.sent_frames.back());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*frame));
    EXPECT_EQ(std::get<protocol::SessionEndedMessage>(*frame).session_id, "srv_test");
}

TEST(AiGatewayWebSocketSessionTest, TransportErrorTerminatesActiveTurnBeforeClose) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_TRUE(session.HandleTransportError("upstream disconnected").ok());

    ASSERT_EQ(connection.sent_frames.size(), 3U);
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::TransportError);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "upstream disconnected");
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");

    const absl::StatusOr<protocol::GatewayMessage> error_frame =
        parse_frame(connection.sent_frames[1]);
    ASSERT_TRUE(error_frame.ok()) << error_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*error_frame));
    EXPECT_EQ(std::get<protocol::ErrorMessage>(*error_frame).code, "transport_error");

    const absl::StatusOr<protocol::GatewayMessage> completed_frame =
        parse_frame(connection.sent_frames[2]);
    ASSERT_TRUE(completed_frame.ok()) << completed_frame.status().ToString();
    EXPECT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*completed_frame));
}

TEST(AiGatewayWebSocketSessionTest, TransportCloseNotifiesInflightTurnWithoutSendingFrames) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    session.HandleTransportClosed();

    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::TransportClosed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "");
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, SendFailureClosesConnectionAndReturnsError) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);
    connection.fail_next_send_ = true;

    const absl::Status status = session.HandleIncomingTextFrame(kSessionStartJson);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "send failed");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionRejectsFurtherIncomingFramesAfterProtocolEnd) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"session.end","session_id":"srv_test"})json")
            .ok());

    const absl::Status incoming = session.HandleIncomingTextFrame(kSessionStartJson);
    const absl::Status transport_error = session.HandleTransportError("late error");

    EXPECT_FALSE(incoming.ok());
    EXPECT_FALSE(transport_error.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Graceful);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(connection.sent_frames.size(), 2U);
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionIgnoresDuplicateTransportClose) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    session.HandleTransportClosed();
    session.HandleTransportClosed();

    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_FALSE(sink.closed_sessions.front().session_started);
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionRejectsFurtherIncomingFramesAfterTransportError) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(session.HandleTransportError("upstream disconnected").ok());

    const absl::Status incoming = session.HandleIncomingTextFrame(kSessionStartJson);
    const absl::Status second_error = session.HandleTransportError("late error");
    session.HandleTransportClosed();

    EXPECT_FALSE(incoming.ok());
    EXPECT_FALSE(second_error.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(connection.sent_frames.size(), 3U);
}

TEST(AiGatewayWebSocketSessionTest, TransportErrorBeforeSessionStartClosesWithoutFrames) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleTransportError("socket failed").ok());

    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    EXPECT_TRUE(connection.sent_frames.empty());
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_FALSE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "socket failed");
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
}

TEST(AiGatewayWebSocketSessionTest,
     TransportErrorAfterSessionStartWithoutTurnClosesWithoutTurnFrames) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session.HandleTransportError("socket failed").ok());

    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "socket failed");
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
}

TEST(AiGatewayWebSocketSessionTest, SendFailureDuringTransportErrorStopsFurtherFrames) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.HandleTransportError("upstream disconnected");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.close_modes.size(), 1U);
    EXPECT_EQ(connection.close_modes.front(), GatewayTransportCloseMode::Force);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, HandleSendFailureClosesSessionWithoutClosingTransport) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    session.HandleSendFailure("send failed");

    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "send failed");
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, ServerShutdownClosesSessionWithoutTransportWarningPath) {
    RecordingWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(kSessionStartJson).ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    session.HandleServerShutdown();

    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::ServerStopping);
    EXPECT_EQ(sink.closed_sessions.front().detail, "server stopping");
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
}

} // namespace
} // namespace isla::server::ai_gateway
