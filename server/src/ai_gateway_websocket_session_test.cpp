#include "ai_gateway_websocket_session.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace protocol = isla::shared::ai_gateway;

absl::StatusOr<protocol::GatewayMessage> parse_frame(const std::string& frame) {
    return protocol::parse_json_message(frame);
}

class FakeWebSocketConnection final : public GatewayWebSocketConnection {
  public:
    [[nodiscard]] absl::Status SendTextFrame(std::string_view frame) override {
        if (fail_next_send_) {
            fail_next_send_ = false;
            return absl::UnavailableError("send failed");
        }
        sent_frames.emplace_back(frame);
        return absl::OkStatus();
    }

    void Close() override {
        ++close_calls;
    }

    bool fail_next_send_ = false;
    int close_calls = 0;
    std::vector<std::string> sent_frames;
};

class RecordingEventSink final : public GatewaySessionEventSink {
  public:
    void OnTurnAccepted(const TurnAcceptedEvent& event) override {
        accepted_turns.push_back(event);
    }

    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override {
        cancel_requests.push_back(event);
    }

    void OnSessionClosed(const SessionClosedEvent& event) override {
        closed_sessions.push_back(event);
    }

    std::vector<TurnAcceptedEvent> accepted_turns;
    std::vector<TurnCancelRequestedEvent> cancel_requests;
    std::vector<SessionClosedEvent> closed_sessions;
};

TEST(AiGatewayWebSocketSessionTest, FactoryGeneratesPerConnectionSessionIds) {
    GatewayWebSocketSessionFactory factory(
        std::make_unique<SequentialSessionIdGenerator>("srv_test_"));
    FakeWebSocketConnection first_connection;
    FakeWebSocketConnection second_connection;

    const std::unique_ptr<GatewayWebSocketSessionAdapter> first =
        factory.CreateSession(first_connection);
    const std::unique_ptr<GatewayWebSocketSessionAdapter> second =
        factory.CreateSession(second_connection);

    EXPECT_EQ(first->session_id(), "srv_test_1");
    EXPECT_EQ(second->session_id(), "srv_test_2");
}

TEST(AiGatewayWebSocketSessionTest, SessionStartWritesSessionStartedFrame) {
    FakeWebSocketConnection connection;
    GatewayWebSocketSessionAdapter session("srv_test", connection);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_EQ(connection.sent_frames.size(), 1U);

    const absl::StatusOr<protocol::GatewayMessage> frame =
        parse_frame(connection.sent_frames.front());
    ASSERT_TRUE(frame.ok()) << frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*frame));
    EXPECT_EQ(std::get<protocol::SessionStartedMessage>(*frame).session_id, "srv_test");
}

TEST(AiGatewayWebSocketSessionTest, AcceptedTurnIsForwardedToEventSink) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_EQ(sink.accepted_turns.size(), 1U);
    EXPECT_EQ(sink.accepted_turns.front().session_id, "srv_test");
    EXPECT_EQ(sink.accepted_turns.front().turn_id, "turn_1");
    EXPECT_EQ(sink.accepted_turns.front().text, "hello");
}

TEST(AiGatewayWebSocketSessionTest, RejectedClientFrameSendsErrorAndKeepsSessionOpen) {
    FakeWebSocketConnection connection;
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

    EXPECT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
}

TEST(AiGatewayWebSocketSessionTest, TurnCancelIsForwardedToEventSink) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
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
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
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
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());

    const absl::Status status = session.EmitTextOutput("turn_1", "stub reply");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "turn_id does not match the active turn");
    EXPECT_FALSE(session.is_closed());
    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.empty());
}

TEST(AiGatewayWebSocketSessionTest, EmitTextOutputSendFailureClosesSession) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.EmitTextOutput("turn_1", "stub reply");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, SessionEndClosesTransportAfterReply) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"session.end","session_id":"srv_test"})json")
            .ok());

    ASSERT_EQ(connection.sent_frames.size(), 2U);
    EXPECT_EQ(connection.close_calls, 1);
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
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());

    ASSERT_TRUE(session.HandleTransportError("upstream disconnected").ok());

    ASSERT_EQ(connection.sent_frames.size(), 3U);
    EXPECT_EQ(connection.close_calls, 1);
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
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
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
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);
    connection.fail_next_send_ = true;

    const absl::Status status =
        session.HandleIncomingTextFrame(R"json({"type":"session.start"})json");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "send failed");
    EXPECT_TRUE(session.is_closed());
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionRejectsFurtherIncomingFramesAfterProtocolEnd) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(
        session.HandleIncomingTextFrame(R"json({"type":"session.end","session_id":"srv_test"})json")
            .ok());

    const absl::Status incoming =
        session.HandleIncomingTextFrame(R"json({"type":"session.start"})json");
    const absl::Status transport_error = session.HandleTransportError("late error");

    EXPECT_FALSE(incoming.ok());
    EXPECT_FALSE(transport_error.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(connection.sent_frames.size(), 2U);
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionIgnoresDuplicateTransportClose) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    session.HandleTransportClosed();
    session.HandleTransportClosed();

    EXPECT_EQ(connection.close_calls, 0);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_FALSE(sink.closed_sessions.front().session_started);
}

TEST(AiGatewayWebSocketSessionTest, ClosedSessionRejectsFurtherIncomingFramesAfterTransportError) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    ASSERT_TRUE(session.HandleTransportError("upstream disconnected").ok());

    const absl::Status incoming =
        session.HandleIncomingTextFrame(R"json({"type":"session.start"})json");
    const absl::Status second_error = session.HandleTransportError("late error");
    session.HandleTransportClosed();

    EXPECT_FALSE(incoming.ok());
    EXPECT_FALSE(second_error.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(connection.sent_frames.size(), 3U);
}

TEST(AiGatewayWebSocketSessionTest, TransportErrorBeforeSessionStartClosesWithoutFrames) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleTransportError("socket failed").ok());

    EXPECT_EQ(connection.close_calls, 1);
    EXPECT_TRUE(connection.sent_frames.empty());
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_FALSE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "socket failed");
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
}

TEST(AiGatewayWebSocketSessionTest,
     TransportErrorAfterSessionStartWithoutTurnClosesWithoutTurnFrames) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session.HandleTransportError("socket failed").ok());

    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_TRUE(sink.closed_sessions.front().session_started);
    EXPECT_EQ(sink.closed_sessions.front().detail, "socket failed");
    EXPECT_FALSE(sink.closed_sessions.front().inflight_turn_id.has_value());
}

TEST(AiGatewayWebSocketSessionTest, SendFailureDuringTransportErrorStopsFurtherFrames) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
    ASSERT_TRUE(session
                    .HandleIncomingTextFrame(
                        R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                    .ok());
    connection.fail_next_send_ = true;

    const absl::Status status = session.HandleTransportError("upstream disconnected");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(connection.close_calls, 1);
    ASSERT_EQ(connection.sent_frames.size(), 1U);
    ASSERT_EQ(sink.closed_sessions.size(), 1U);
    EXPECT_EQ(sink.closed_sessions.front().reason, SessionCloseReason::SendFailed);
    ASSERT_TRUE(sink.closed_sessions.front().inflight_turn_id.has_value());
    EXPECT_EQ(*sink.closed_sessions.front().inflight_turn_id, "turn_1");
}

TEST(AiGatewayWebSocketSessionTest, ServerShutdownClosesSessionWithoutTransportWarningPath) {
    FakeWebSocketConnection connection;
    RecordingEventSink sink;
    GatewayWebSocketSessionAdapter session("srv_test", connection, &sink);

    ASSERT_TRUE(session.HandleIncomingTextFrame(R"json({"type":"session.start"})json").ok());
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
