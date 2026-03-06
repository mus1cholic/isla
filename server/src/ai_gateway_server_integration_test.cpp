#include "ai_gateway_server.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>

#include <gtest/gtest.h>

#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace protocol = isla::shared::ai_gateway;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

class RecordingApplicationSink final : public GatewayApplicationEventSink {
  public:
    void OnTurnAccepted(const TurnAcceptedEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepted_turns.push_back(event);
        }
        cv_.notify_all();
    }

    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_requests.push_back(event);
        }
        cv_.notify_all();
    }

    void OnSessionClosed(const SessionClosedEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_sessions.push_back(event);
        }
        cv_.notify_all();
    }

    template <typename Predicate> bool WaitFor(Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, predicate);
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<TurnAcceptedEvent> accepted_turns;
    std::vector<TurnCancelRequestedEvent> cancel_requests;
    std::vector<SessionClosedEvent> closed_sessions;
};

class RealWebSocketClient {
  public:
    RealWebSocketClient() : websocket_(io_context_) {}

    [[nodiscard]] absl::Status Connect(std::uint16_t port) {
        boost::system::error_code error;
        tcp::resolver resolver(io_context_);
        const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), error);
        if (error) {
            return absl::UnavailableError(error.message());
        }

        const tcp::endpoint connected_endpoint =
            asio::connect(websocket_.next_layer(), endpoints, error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        static_cast<void>(connected_endpoint);

        websocket_.handshake("127.0.0.1", "/", error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status SendJson(std::string_view json) {
        boost::system::error_code error;
        const std::size_t bytes_written =
            websocket_.write(asio::buffer(json.data(), json.size()), error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        if (bytes_written != json.size()) {
            return absl::UnavailableError("client websocket write sent a partial frame");
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status SendBinary(std::string_view bytes) {
        boost::system::error_code error;
        websocket_.binary(true);
        const std::size_t bytes_written =
            websocket_.write(asio::buffer(bytes.data(), bytes.size()), error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        if (bytes_written != bytes.size()) {
            return absl::UnavailableError("client websocket write sent a partial frame");
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<protocol::GatewayMessage> ReadJsonFrame() {
        beast::flat_buffer buffer;
        boost::system::error_code error;
        const std::size_t bytes_read = websocket_.read(buffer, error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        static_cast<void>(bytes_read);
        return protocol::parse_json_message(beast::buffers_to_string(buffer.data()));
    }

    void CloseTransport() {
        boost::system::error_code error;
        websocket_.close(websocket::close_code::normal, error);
    }

  private:
    asio::io_context io_context_;
    websocket::stream<tcp::socket> websocket_;
};

class RawTcpClient {
  public:
    RawTcpClient() : socket_(io_context_) {}

    [[nodiscard]] absl::Status Connect(std::uint16_t port) {
        boost::system::error_code error;
        tcp::resolver resolver(io_context_);
        const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), error);
        if (error) {
            return absl::UnavailableError(error.message());
        }

        const tcp::endpoint connected_endpoint = asio::connect(socket_, endpoints, error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        static_cast<void>(connected_endpoint);
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status Send(std::string_view bytes) {
        boost::system::error_code error;
        const std::size_t bytes_written =
            asio::write(socket_, asio::buffer(bytes.data(), bytes.size()), error);
        if (error) {
            return absl::UnavailableError(error.message());
        }
        if (bytes_written != bytes.size()) {
            return absl::UnavailableError("raw tcp write sent a partial payload");
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<std::string> ReadSome() {
        std::array<char, 512> buffer{};
        boost::system::error_code error;
        const std::size_t bytes_read = socket_.read_some(asio::buffer(buffer), error);
        if (error == asio::error::eof) {
            return std::string();
        }
        if (error) {
            return absl::UnavailableError(error.message());
        }
        return std::string(buffer.data(), bytes_read);
    }

    void Close() {
        boost::system::error_code error;
        socket_.close(error);
    }

  private:
    asio::io_context io_context_;
    tcp::socket socket_;
};

class GatewayServerTest : public ::testing::Test {
  protected:
    GatewayServerTest()
        : server_(GatewayServerConfig{ .bind_host = "127.0.0.1", .port = 0, .listen_backlog = 4 },
                  &sink_, std::make_unique<SequentialSessionIdGenerator>("srv_it_")) {}

    void SetUp() override {
        ASSERT_TRUE(server_.Start().ok());
        ASSERT_TRUE(server_.is_running());
        ASSERT_NE(server_.bound_port(), 0);
    }

    void TearDown() override {
        server_.Stop();
    }

    RecordingApplicationSink sink_;
    GatewayServer server_;
};

TEST_F(GatewayServerTest, RealSocketTurnIngressReachesTypedApplicationSink) {
    {
        RealWebSocketClient client;
        ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
        ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

        const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
        ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
        const std::string session_id =
            std::get<protocol::SessionStartedMessage>(*started_frame).session_id;
        EXPECT_EQ(session_id, "srv_it_1");

        ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 1U; }));
        const std::shared_ptr<GatewayLiveSession> live_session =
            server_.session_registry().FindSession(session_id);
        ASSERT_NE(live_session, nullptr);
        EXPECT_EQ(live_session->session_id(), session_id);
        EXPECT_FALSE(live_session->is_closed());

        ASSERT_TRUE(
            client
                .SendJson(
                    R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
                .ok());
        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.accepted_turns.size() == 1U; }));
        EXPECT_EQ(sink_.accepted_turns.front().session_id, session_id);
        EXPECT_EQ(sink_.accepted_turns.front().turn_id, "turn_1");
        EXPECT_EQ(sink_.accepted_turns.front().text, "hello gateway");

        ASSERT_TRUE(client.SendJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json").ok());
        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.cancel_requests.size() == 1U; }));
        EXPECT_EQ(sink_.cancel_requests.front().session_id, session_id);
        EXPECT_EQ(sink_.cancel_requests.front().turn_id, "turn_1");

        client.CloseTransport();
        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
        EXPECT_EQ(sink_.closed_sessions.front().session_id, session_id);
        EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::TransportClosed);
        ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
    }

    {
        RealWebSocketClient client;
        ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
        ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

        const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
        ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
        const std::string session_id =
            std::get<protocol::SessionStartedMessage>(*started_frame).session_id;
        EXPECT_EQ(session_id, "srv_it_2");

        const std::string end_message =
            std::string("{\"type\":\"session.end\",\"session_id\":\"") + session_id + "\"}";
        const absl::Status end_status = client.SendJson(end_message);
        ASSERT_TRUE(end_status.ok()) << end_status;

        const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
        ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));
        EXPECT_EQ(std::get<protocol::SessionEndedMessage>(*ended_frame).session_id, session_id);

        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 2U; }));
        EXPECT_EQ(sink_.closed_sessions.back().session_id, session_id);
        EXPECT_EQ(sink_.closed_sessions.back().reason, SessionCloseReason::ProtocolEnded);
        ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
    }
}

TEST_F(GatewayServerTest, RejectsInvalidHandshakeWithoutRegisteringSession) {
    RawTcpClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.Send("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").ok());

    const absl::StatusOr<std::string> response = client.ReadSome();
    ASSERT_TRUE(response.ok()) << response.status();
    EXPECT_NE(response->find("HTTP/1.1"), std::string::npos);
    EXPECT_EQ(server_.session_registry().SessionCount(), 0U);
    EXPECT_TRUE(sink_.closed_sessions.empty());
}

TEST_F(GatewayServerTest, RejectsBinaryFramesAfterSessionStartAndClosesTransport) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(client.SendBinary("010203").ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_EQ(sink_.closed_sessions.front().session_id, session_id);
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::TransportError);
    EXPECT_EQ(sink_.closed_sessions.front().detail, "unsupported websocket opcode");
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
}

TEST_F(GatewayServerTest, ProtocolErrorsOverRealSocketSendErrorAndKeepSessionOpen) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson("{not valid json").ok());

    const absl::StatusOr<protocol::GatewayMessage> error_frame = client.ReadJsonFrame();
    ASSERT_TRUE(error_frame.ok()) << error_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*error_frame));
    EXPECT_EQ(std::get<protocol::ErrorMessage>(*error_frame).code, "bad_request");

    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());
    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 1U; }));
    EXPECT_TRUE(sink_.accepted_turns.empty());
    EXPECT_TRUE(sink_.closed_sessions.empty());
}

TEST_F(GatewayServerTest, StopClosesStartedSessionAndClearsRegistry) {
    {
        RealWebSocketClient client;
        ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
        ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

        const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
        ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
        const std::string session_id =
            std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

        server_.Stop();
        EXPECT_FALSE(server_.is_running());
        EXPECT_EQ(server_.session_registry().SessionCount(), 0U);
        ASSERT_TRUE(sink_.WaitFor([&] { return !sink_.closed_sessions.empty(); }));
        EXPECT_EQ(sink_.closed_sessions.back().session_id, session_id);
        EXPECT_EQ(sink_.closed_sessions.back().reason, SessionCloseReason::ServerStopping);

        client.CloseTransport();
    }
}

TEST_F(GatewayServerTest, StopWhileIdleSucceedsWithoutSessions) {
    EXPECT_TRUE(sink_.closed_sessions.empty());
    EXPECT_EQ(server_.session_registry().SessionCount(), 0U);

    server_.Stop();
    EXPECT_FALSE(server_.is_running());
    EXPECT_EQ(server_.session_registry().SessionCount(), 0U);
    EXPECT_TRUE(sink_.closed_sessions.empty());
}

TEST_F(GatewayServerTest, DisconnectBeforeSessionStartDoesNotLeaveStaleRegistryState) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 1U; }));

    client.CloseTransport();

    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_FALSE(sink_.closed_sessions.front().session_started);
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::TransportClosed);
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
}

TEST_F(GatewayServerTest, ReapsSessionAfterProtocolEndedClose) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    const std::string end_message =
        std::string("{\"type\":\"session.end\",\"session_id\":\"") + session_id + "\"}";
    ASSERT_TRUE(client.SendJson(end_message).ok());

    const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
    ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));

    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::ProtocolEnded);
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
}

TEST_F(GatewayServerTest, ReapsSessionAfterTransportClose) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    client.CloseTransport();

    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::TransportClosed);
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
}

TEST_F(GatewayServerTest, ReapsManyShortLivedSessionsWithoutAccumulatingState) {
    constexpr int kSessionCount = 12;

    for (int i = 0; i < kSessionCount; ++i) {
        RealWebSocketClient client;
        ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
        ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

        const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
        ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
        const std::string session_id =
            std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

        if ((i % 2) == 0) {
            const std::string end_message =
                std::string("{\"type\":\"session.end\",\"session_id\":\"") + session_id + "\"}";
            ASSERT_TRUE(client.SendJson(end_message).ok());
            const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
            ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
            ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));
        } else {
            client.CloseTransport();
        }

        ASSERT_TRUE(sink_.WaitFor([&] {
            return sink_.closed_sessions.size() == static_cast<std::size_t>(i + 1);
        }));
        ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
    }

    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());
    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
}

TEST(AiGatewayServerLifecycleTest, RejectsInvalidBindHost) {
    GatewayServer server(
        GatewayServerConfig{ .bind_host = "not-an-ip", .port = 0, .listen_backlog = 4 });

    const absl::Status status = server.Start();

    EXPECT_FALSE(status.ok());
    EXPECT_NE(std::string(status.message()).find("invalid bind_host"), std::string::npos);
}

TEST(AiGatewayServerLifecycleTest, RejectsDoubleStart) {
    GatewayServer server(
        GatewayServerConfig{ .bind_host = "127.0.0.1", .port = 0, .listen_backlog = 4 });
    ASSERT_TRUE(server.Start().ok());

    const absl::Status status = server.Start();

    EXPECT_FALSE(status.ok());
    EXPECT_NE(std::string(status.message()).find("already running"), std::string::npos);
    server.Stop();
}

TEST(AiGatewayServerLifecycleTest, RejectsBindingToInUsePort) {
#ifdef _WIN32
    GTEST_SKIP() << "Windows allows this bind pattern with reuse_address(true).";
#endif
    GatewayServer first(
        GatewayServerConfig{ .bind_host = "127.0.0.1", .port = 0, .listen_backlog = 4 });
    ASSERT_TRUE(first.Start().ok());

    GatewayServer second(GatewayServerConfig{
        .bind_host = "127.0.0.1", .port = first.bound_port(), .listen_backlog = 4 });
    const absl::Status status = second.Start();

    EXPECT_FALSE(status.ok());
    EXPECT_NE(std::string(status.message()).find("bind failed"), std::string::npos);
    first.Stop();
}

} // namespace
} // namespace isla::server::ai_gateway
