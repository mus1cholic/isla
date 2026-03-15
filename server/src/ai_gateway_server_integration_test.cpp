#include "isla/server/ai_gateway_server.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/shared/ai_gateway_protocol.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace protocol = isla::shared::ai_gateway;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

template <typename StartFn> absl::Status await_emit(StartFn&& start) {
    auto promise = std::make_shared<std::promise<absl::Status>>();
    std::future<absl::Status> future = promise->get_future();
    start([promise](absl::Status status) { promise->set_value(std::move(status)); });
    if (future.wait_for(2s) != std::future_status::ready) {
        return absl::DeadlineExceededError("timed out waiting for async emit completion");
    }
    return future.get();
}

std::shared_ptr<test::FakeOpenAiResponsesClient> MakeEchoOpenAiResponsesClient() {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            const std::string text =
                std::string("stub echo: ") + test::ExtractLatestPromptLine(request.user_text);
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
            if (!delta_status.ok()) {
                return delta_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_test",
            });
        });
}

class RecordingApplicationSink final : public GatewayApplicationEventSink {
  public:
    void OnSessionStarted(const SessionStartedEvent& event) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            started_sessions.push_back(event);
        }
        cv_.notify_all();
    }

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
    std::vector<SessionStartedEvent> started_sessions;
    std::vector<TurnAcceptedEvent> accepted_turns;
    std::vector<TurnCancelRequestedEvent> cancel_requests;
    std::vector<SessionClosedEvent> closed_sessions;
};

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    void OnTurnAccepted(const TurnTelemetryContext& context) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepted_turns.push_back(
                { .session_id = context.session_id, .turn_id = context.turn_id });
        }
        cv_.notify_all();
    }

    bool WaitForAcceptedTurnCount(std::size_t expected_count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] { return accepted_turns.size() >= expected_count; });
    }

    [[nodiscard]] std::vector<TurnAcceptedEvent> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepted_turns;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable std::vector<TurnAcceptedEvent> accepted_turns;
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

    [[nodiscard]] absl::StatusOr<websocket::close_reason>
    ReadCloseReason(std::chrono::milliseconds timeout = 2s) {
        beast::flat_buffer buffer;
        std::optional<absl::StatusOr<websocket::close_reason>> result;

        websocket::stream_base::timeout timeout_options =
            websocket::stream_base::timeout::suggested(beast::role_type::client);
        timeout_options.idle_timeout = timeout;
        timeout_options.keep_alive_pings = false;
        websocket_.set_option(timeout_options);

        websocket_.async_read(buffer, [this, &result](const boost::system::error_code& error,
                                                      std::size_t bytes_read) {
            static_cast<void>(bytes_read);
            if (error == websocket::error::closed) {
                result.emplace(websocket_.reason());
                return;
            }
            if (error == beast::error::timeout) {
                result.emplace(
                    absl::DeadlineExceededError("timed out waiting for websocket close frame"));
                return;
            }
            if (error) {
                result.emplace(absl::UnavailableError(error.message()));
                return;
            }
            result.emplace(absl::InternalError("expected websocket close frame"));
        });

        io_context_.restart();
        io_context_.run();
        if (!result.has_value()) {
            return absl::InternalError("websocket close read completed without a result");
        }
        return *result;
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

class GatewayStubResponderServerTest : public ::testing::Test {
  protected:
    GatewayStubResponderServerTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 100ms,
              .openai_client = MakeEchoOpenAiResponsesClient(),
          }),
          server_(GatewayServerConfig{ .bind_host = "127.0.0.1", .port = 0, .listen_backlog = 4 },
                  &responder_, std::make_unique<SequentialSessionIdGenerator>("srv_stub_")) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    void SetUp() override {
        ASSERT_TRUE(server_.Start().ok());
        ASSERT_TRUE(server_.is_running());
        ASSERT_NE(server_.bound_port(), 0);
    }

    void TearDown() override {
        server_.Stop();
    }

    GatewayStubResponder responder_;
    GatewayServer server_;
};

class GatewayServerShortShutdownGraceTest : public ::testing::Test {
  protected:
    GatewayServerShortShutdownGraceTest()
        : server_(
              GatewayServerConfig{
                  .bind_host = "127.0.0.1",
                  .port = 0,
                  .listen_backlog = 4,
                  .shutdown_write_grace_period = 50ms,
              },
              &sink_, std::make_unique<SequentialSessionIdGenerator>("srv_stop_")) {}

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

        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.started_sessions.size() == 1U; }));
        EXPECT_EQ(sink_.started_sessions.front().session_id, session_id);
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
        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.started_sessions.size() == 2U; }));
        EXPECT_EQ(sink_.started_sessions.back().session_id, session_id);

        const std::string end_message =
            std::string("{\"type\":\"session.end\",\"session_id\":\"") + session_id + "\"}";
        const absl::Status end_status = client.SendJson(end_message);
        ASSERT_TRUE(end_status.ok()) << end_status;

        const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
        ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));
        EXPECT_EQ(std::get<protocol::SessionEndedMessage>(*ended_frame).session_id, session_id);
        const absl::StatusOr<websocket::close_reason> close_reason = client.ReadCloseReason();
        ASSERT_TRUE(close_reason.ok()) << close_reason.status().ToString();
        EXPECT_EQ(close_reason->code, websocket::close_code::normal);

        ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 2U; }));
        EXPECT_EQ(sink_.closed_sessions.back().session_id, session_id);
        EXPECT_EQ(sink_.closed_sessions.back().reason, SessionCloseReason::ProtocolEnded);
        ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
    }
}

TEST(AiGatewayServerIntegrationTest, RealSocketTurnIngressUsesConfiguredTelemetrySink) {
    RecordingApplicationSink sink;
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    GatewayServer server(
        GatewayServerConfig{
            .bind_host = "127.0.0.1",
            .port = 0,
            .listen_backlog = 4,
            .telemetry_sink = telemetry_sink,
        },
        &sink, std::make_unique<SequentialSessionIdGenerator>("srv_tel_"));
    ASSERT_TRUE(server.Start().ok());
    ASSERT_TRUE(server.is_running());
    ASSERT_NE(server.bound_port(), 0);

    {
        RealWebSocketClient client;
        ASSERT_TRUE(client.Connect(server.bound_port()).ok());
        ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

        const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
        ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
        ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
        const std::string session_id =
            std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

        ASSERT_TRUE(
            client.SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello"})json")
                .ok());
        ASSERT_TRUE(sink.WaitFor([&] { return sink.accepted_turns.size() == 1U; }));
        ASSERT_EQ(sink.accepted_turns.size(), 1U);
        ASSERT_NE(sink.accepted_turns.front().telemetry_context, nullptr);
        EXPECT_EQ(sink.accepted_turns.front().telemetry_context->session_id, session_id);
        EXPECT_EQ(sink.accepted_turns.front().telemetry_context->turn_id, "turn_1");
        EXPECT_EQ(sink.accepted_turns.front().telemetry_context->sink, telemetry_sink);

        ASSERT_TRUE(telemetry_sink->WaitForAcceptedTurnCount(1U));
        const std::vector<TurnAcceptedEvent> telemetry_turns = telemetry_sink->snapshot();
        ASSERT_EQ(telemetry_turns.size(), 1U);
        EXPECT_EQ(telemetry_turns.front().session_id, session_id);
        EXPECT_EQ(telemetry_turns.front().turn_id, "turn_1");

        client.CloseTransport();
        ASSERT_TRUE(sink.WaitFor([&] { return sink.closed_sessions.size() == 1U; }));
    }

    server.Stop();
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

TEST_F(GatewayServerTest, OversizedTextInputReturnsProtocolErrorAndKeepsSessionOpen) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    const std::string oversized_text(kMaxTextInputBytes + 1U, 'x');
    const std::string oversized_message =
        std::string("{\"type\":\"text.input\",\"turn_id\":\"turn_1\",\"text\":\"") +
        oversized_text + "\"}";
    ASSERT_TRUE(client.SendJson(oversized_message).ok());

    const absl::StatusOr<protocol::GatewayMessage> error_frame = client.ReadJsonFrame();
    ASSERT_TRUE(error_frame.ok()) << error_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*error_frame));
    const auto& error = std::get<protocol::ErrorMessage>(*error_frame);
    EXPECT_EQ(error.code, "bad_request");
    ASSERT_TRUE(error.turn_id.has_value());
    EXPECT_EQ(*error.turn_id, "turn_1");
    EXPECT_EQ(error.message, "text.input text exceeds maximum length");
    EXPECT_TRUE(sink_.accepted_turns.empty());
    EXPECT_TRUE(sink_.closed_sessions.empty());

    const std::string end_message =
        std::string("{\"type\":\"session.end\",\"session_id\":\"") + session_id + "\"}";
    ASSERT_TRUE(client.SendJson(end_message).ok());
    const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
    ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));
}

TEST_F(GatewayServerTest, OversizedWebSocketMessageClosesSessionAsTransportError) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    const std::string oversized_payload(kMaxInboundWebSocketMessageBytes + 1U, 'x');
    ASSERT_TRUE(client.SendJson(oversized_payload).ok());

    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_EQ(sink_.closed_sessions.front().session_id, session_id);
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::TransportError);
    EXPECT_EQ(sink_.closed_sessions.front().detail, "websocket message too large");
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));
}

TEST_F(GatewayServerTest, ServerOwnedWritesReachIdleClientWhileAsyncReadIsPending) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.accepted_turns.size() == 1U; }));

    const std::shared_ptr<GatewayLiveSession> live_session =
        server_.session_registry().FindSession(session_id);
    ASSERT_NE(live_session, nullptr);
    ASSERT_TRUE(await_emit([&](GatewayEmitCallback on_complete) {
                    live_session->AsyncEmitTextOutput("turn_1", "stub reply",
                                                      std::move(on_complete));
                }).ok());
    ASSERT_TRUE(await_emit([&](GatewayEmitCallback on_complete) {
                    live_session->AsyncEmitTurnCompleted("turn_1", std::move(on_complete));
                }).ok());

    const absl::StatusOr<protocol::GatewayMessage> text_output = client.ReadJsonFrame();
    ASSERT_TRUE(text_output.ok()) << text_output.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*text_output));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_output).turn_id, "turn_1");
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_output).text, "stub reply");

    const absl::StatusOr<protocol::GatewayMessage> turn_completed = client.ReadJsonFrame();
    ASSERT_TRUE(turn_completed.ok()) << turn_completed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*turn_completed));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*turn_completed).turn_id, "turn_1");

    client.CloseTransport();
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
}

TEST_F(GatewayServerTest, ServerOwnedEmitFailsCleanlyAfterTransportClose) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.accepted_turns.size() == 1U; }));

    const std::shared_ptr<GatewayLiveSession> live_session =
        server_.session_registry().FindSession(session_id);
    ASSERT_NE(live_session, nullptr);

    client.CloseTransport();

    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    ASSERT_TRUE(sink_.WaitFor([&] { return server_.session_registry().SessionCount() == 0U; }));

    const absl::Status status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTextOutput("turn_1", "late reply", std::move(on_complete));
    });
    EXPECT_FALSE(status.ok());
    EXPECT_NE(std::string(status.message()).find("closed"), std::string::npos);
}

TEST_F(GatewayServerTest, ServerOwnedTurnCancelledReachesClientAfterAcceptedCancel) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.accepted_turns.size() == 1U; }));

    ASSERT_TRUE(client.SendJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json").ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.cancel_requests.size() == 1U; }));

    const std::shared_ptr<GatewayLiveSession> live_session =
        server_.session_registry().FindSession(session_id);
    ASSERT_NE(live_session, nullptr);
    ASSERT_TRUE(await_emit([&](GatewayEmitCallback on_complete) {
                    live_session->AsyncEmitTurnCancelled("turn_1", std::move(on_complete));
                }).ok());

    const absl::StatusOr<protocol::GatewayMessage> cancelled_frame = client.ReadJsonFrame();
    ASSERT_TRUE(cancelled_frame.ok()) << cancelled_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCancelledMessage>(*cancelled_frame));
    EXPECT_EQ(std::get<protocol::TurnCancelledMessage>(*cancelled_frame).turn_id, "turn_1");

    client.CloseTransport();
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
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

TEST_F(GatewayServerShortShutdownGraceTest, StopReturnsPromptlyWhenPendingWriteDoesNotDrain) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.accepted_turns.size() == 1U; }));

    const std::shared_ptr<GatewayLiveSession> live_session =
        server_.session_registry().FindSession(session_id);
    ASSERT_NE(live_session, nullptr);

    std::promise<absl::Status> text_emit_promise;
    std::future<absl::Status> text_emit_future = text_emit_promise.get_future();
    live_session->AsyncEmitTextOutput(std::string("turn_1"), std::string("seed"),
                                      [&text_emit_promise](absl::Status status) mutable {
                                          text_emit_promise.set_value(std::move(status));
                                      });
    ASSERT_EQ(text_emit_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(text_emit_future.get().ok());

    // This uses a large pending socket write to exercise the shutdown grace-period fallback
    // because the current integration harness does not provide a fully synthetic stalled-write
    // path. Use audio.output here so the test still stresses the websocket send queue without
    // violating the bounded text.output contract.
    std::promise<absl::Status> emit_promise;
    std::future<absl::Status> emit_future = emit_promise.get_future();
    live_session->AsyncEmitAudioOutput(std::string("turn_1"), std::string("audio/pcm"),
                                       std::string(8U * 1024U * 1024U, 'x'),
                                       [&emit_promise](absl::Status status) mutable {
                                           emit_promise.set_value(std::move(status));
                                       });
    ASSERT_EQ(emit_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(emit_future.get().ok());

    auto stop_future = std::async(std::launch::async, [this] { server_.Stop(); });
    EXPECT_EQ(stop_future.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(server_.is_running());
}

TEST_F(GatewayStubResponderServerTest, StubResponderReturnsFinalTextAndCompletion) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());

    const absl::StatusOr<protocol::GatewayMessage> text_output = client.ReadJsonFrame();
    ASSERT_TRUE(text_output.ok()) << text_output.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*text_output));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_output).turn_id, "turn_1");
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*text_output).text, "stub echo: hello gateway");

    const absl::StatusOr<protocol::GatewayMessage> turn_completed = client.ReadJsonFrame();
    ASSERT_TRUE(turn_completed.ok()) << turn_completed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*turn_completed));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*turn_completed).turn_id, "turn_1");
}

TEST(AiGatewayServerIntegrationTest, MultiDeltaProviderStillProducesSingleFinalTextOutput) {
    auto multi_delta_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            const std::string latest_text = test::ExtractLatestPromptLine(request.user_text);
            const absl::Status first_status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = "stub ",
            });
            if (!first_status.ok()) {
                return first_status;
            }
            const absl::Status second_status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = "echo: " + latest_text,
            });
            if (!second_status.ok()) {
                return second_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_test",
            });
        });
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = multi_delta_client,
    });
    GatewayServer server(
        GatewayServerConfig{ .bind_host = "127.0.0.1", .port = 0, .listen_backlog = 4 }, &responder,
        std::make_unique<SequentialSessionIdGenerator>("srv_multi_"));
    responder.AttachSessionRegistry(&server.session_registry());

    ASSERT_TRUE(server.Start().ok());

    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());

    const absl::StatusOr<protocol::GatewayMessage> first_frame = client.ReadJsonFrame();
    ASSERT_TRUE(first_frame.ok()) << first_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*first_frame));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*first_frame).turn_id, "turn_1");
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*first_frame).text, "stub echo: hello gateway");

    const absl::StatusOr<protocol::GatewayMessage> second_frame = client.ReadJsonFrame();
    ASSERT_TRUE(second_frame.ok()) << second_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*second_frame));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*second_frame).turn_id, "turn_1");

    client.CloseTransport();
    server.Stop();
}

TEST_F(GatewayStubResponderServerTest, StubResponderTerminatesAcceptedCancellation) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"turn.cancel","turn_id":"turn_1"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> cancelled_frame = client.ReadJsonFrame();
    ASSERT_TRUE(cancelled_frame.ok()) << cancelled_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCancelledMessage>(*cancelled_frame));
    EXPECT_EQ(std::get<protocol::TurnCancelledMessage>(*cancelled_frame).turn_id, "turn_1");
}

TEST_F(GatewayStubResponderServerTest, StubResponderSupportsSequentialTurnsOnOneSession) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    ASSERT_TRUE(
        client.SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"first"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> first_text = client.ReadJsonFrame();
    ASSERT_TRUE(first_text.ok()) << first_text.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*first_text));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*first_text).text, "stub echo: first");

    const absl::StatusOr<protocol::GatewayMessage> first_completed = client.ReadJsonFrame();
    ASSERT_TRUE(first_completed.ok()) << first_completed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*first_completed));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*first_completed).turn_id, "turn_1");

    ASSERT_TRUE(
        client.SendJson(R"json({"type":"text.input","turn_id":"turn_2","text":"second"})json")
            .ok());

    const absl::StatusOr<protocol::GatewayMessage> second_text = client.ReadJsonFrame();
    ASSERT_TRUE(second_text.ok()) << second_text.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*second_text));
    EXPECT_EQ(std::get<protocol::TextOutputMessage>(*second_text).text, "stub echo: second");

    const absl::StatusOr<protocol::GatewayMessage> second_completed = client.ReadJsonFrame();
    ASSERT_TRUE(second_completed.ok()) << second_completed.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*second_completed));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*second_completed).turn_id, "turn_2");
}

TEST_F(GatewayStubResponderServerTest, SessionEndSucceedsAfterStubTurnCompletes) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());

    const absl::StatusOr<protocol::GatewayMessage> text_output = client.ReadJsonFrame();
    ASSERT_TRUE(text_output.ok()) << text_output.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TextOutputMessage>(*text_output));

    const absl::StatusOr<protocol::GatewayMessage> completed_frame = client.ReadJsonFrame();
    ASSERT_TRUE(completed_frame.ok()) << completed_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*completed_frame));

    ASSERT_TRUE(client
                    .SendJson(std::string("{\"type\":\"session.end\",\"session_id\":\"") +
                              session_id + "\"}")
                    .ok());

    const absl::StatusOr<protocol::GatewayMessage> ended_frame = client.ReadJsonFrame();
    ASSERT_TRUE(ended_frame.ok()) << ended_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionEndedMessage>(*ended_frame));
    EXPECT_EQ(std::get<protocol::SessionEndedMessage>(*ended_frame).session_id, session_id);
}

TEST_F(GatewayStubResponderServerTest, StopEmitsServerStoppingErrorAndCompletionForAcceptedTurn) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));

    ASSERT_TRUE(
        client
            .SendJson(R"json({"type":"text.input","turn_id":"turn_1","text":"hello gateway"})json")
            .ok());

    ASSERT_TRUE(responder_.WaitForAcceptedTurns(1));

    std::thread stop_thread([this] { server_.Stop(); });

    const absl::StatusOr<protocol::GatewayMessage> error_frame = client.ReadJsonFrame();
    ASSERT_TRUE(error_frame.ok()) << error_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorMessage>(*error_frame));
    EXPECT_EQ(std::get<protocol::ErrorMessage>(*error_frame).code, "server_stopping");
    ASSERT_TRUE(std::get<protocol::ErrorMessage>(*error_frame).turn_id.has_value());
    EXPECT_EQ(*std::get<protocol::ErrorMessage>(*error_frame).turn_id, "turn_1");

    const absl::StatusOr<protocol::GatewayMessage> completed_frame = client.ReadJsonFrame();
    ASSERT_TRUE(completed_frame.ok()) << completed_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::TurnCompletedMessage>(*completed_frame));
    EXPECT_EQ(std::get<protocol::TurnCompletedMessage>(*completed_frame).turn_id, "turn_1");

    stop_thread.join();
    EXPECT_FALSE(server_.is_running());
}

TEST_F(GatewayServerTest, ServerStopClosesIdleWebSocketWithGoingAway) {
    RealWebSocketClient client;
    ASSERT_TRUE(client.Connect(server_.bound_port()).ok());
    ASSERT_TRUE(client.SendJson(R"json({"type":"session.start"})json").ok());

    const absl::StatusOr<protocol::GatewayMessage> started_frame = client.ReadJsonFrame();
    ASSERT_TRUE(started_frame.ok()) << started_frame.status().ToString();
    ASSERT_TRUE(std::holds_alternative<protocol::SessionStartedMessage>(*started_frame));
    const std::string session_id =
        std::get<protocol::SessionStartedMessage>(*started_frame).session_id;

    std::thread stop_thread([this] { server_.Stop(); });

    const absl::StatusOr<websocket::close_reason> close_reason = client.ReadCloseReason();
    ASSERT_TRUE(close_reason.ok()) << close_reason.status().ToString();
    EXPECT_EQ(close_reason->code, websocket::close_code::going_away);

    stop_thread.join();
    EXPECT_FALSE(server_.is_running());
    ASSERT_TRUE(sink_.WaitFor([&] { return sink_.closed_sessions.size() == 1U; }));
    EXPECT_EQ(sink_.closed_sessions.front().session_id, session_id);
    EXPECT_EQ(sink_.closed_sessions.front().reason, SessionCloseReason::ServerStopping);
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
    const absl::StatusOr<websocket::close_reason> close_reason = client.ReadCloseReason();
    ASSERT_TRUE(close_reason.ok()) << close_reason.status().ToString();
    EXPECT_EQ(close_reason->code, websocket::close_code::normal);

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
            const absl::StatusOr<websocket::close_reason> close_reason = client.ReadCloseReason();
            ASSERT_TRUE(close_reason.ok()) << close_reason.status().ToString();
            EXPECT_EQ(close_reason->code, websocket::close_code::normal);
        } else {
            client.CloseTransport();
        }

        ASSERT_TRUE(sink_.WaitFor(
            [&] { return sink_.closed_sessions.size() == static_cast<std::size_t>(i + 1); }));
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
