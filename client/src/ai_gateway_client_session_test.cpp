#include "ai_gateway_client_session.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::client {
namespace {

using namespace std::chrono_literals;
namespace protocol = isla::shared::ai_gateway;
using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::ai_gateway::SequentialSessionIdGenerator;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        const absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = "stub echo: " + request.user_text,
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = "resp_client_test",
        });
    }
};

std::shared_ptr<FakeOpenAiResponsesClient> MakeEchoOpenAiResponsesClient() {
    return std::make_shared<FakeOpenAiResponsesClient>();
}

class FailingOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& /*request*/,
                   const OpenAiResponsesEventCallback& /*on_event*/) const override {
        return absl::UnavailableError("provider unavailable");
    }
};

std::shared_ptr<FailingOpenAiResponsesClient> MakeFailingOpenAiResponsesClient() {
    return std::make_shared<FailingOpenAiResponsesClient>();
}

class RawTcpHandshakeRejectServer {
  public:
    RawTcpHandshakeRejectServer() : acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~RawTcpHandshakeRejectServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

  private:
    void Stop() {
        if (!stopped_) {
            stopped_ = true;
            boost::system::error_code error;
            acceptor_.close(error);
            io_context_.stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            tcp::socket socket(io_context_);
            acceptor_.accept(socket);
            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
        }
    }

    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    bool stopped_ = false;
    std::uint16_t port_ = 0;
};

class RecordingClientEvents {
  public:
    void RecordMessage(const protocol::GatewayMessage& message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(message);
        }
        cv_.notify_all();
    }

    void RecordTransportClosed(absl::Status status) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transport_closed_status_ = std::move(status);
        }
        cv_.notify_all();
    }

    bool WaitForTextOutputAndCompletion(std::string_view turn_id, std::string_view text) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] {
            bool saw_text_output = false;
            bool saw_turn_completed = false;
            for (const auto& message : messages_) {
                if (std::holds_alternative<protocol::TextOutputMessage>(message)) {
                    const auto& text_output = std::get<protocol::TextOutputMessage>(message);
                    saw_text_output = text_output.turn_id == turn_id && text_output.text == text;
                }
                if (std::holds_alternative<protocol::TurnCompletedMessage>(message)) {
                    saw_turn_completed =
                        std::get<protocol::TurnCompletedMessage>(message).turn_id == turn_id;
                }
            }
            return saw_text_output && saw_turn_completed;
        });
    }

    bool WaitForTurnCancelled(std::string_view turn_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] {
            for (const auto& message : messages_) {
                if (std::holds_alternative<protocol::TurnCancelledMessage>(message) &&
                    std::get<protocol::TurnCancelledMessage>(message).turn_id == turn_id) {
                    return true;
                }
            }
            return false;
        });
    }

    bool WaitForSessionEnded(std::string_view session_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] {
            for (const auto& message : messages_) {
                if (std::holds_alternative<protocol::SessionEndedMessage>(message) &&
                    std::get<protocol::SessionEndedMessage>(message).session_id == session_id) {
                    return true;
                }
            }
            return false;
        });
    }

    bool WaitForErrorAndCompletion(std::string_view turn_id, std::string_view code,
                                   std::string_view message) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] {
            bool saw_error = false;
            bool saw_turn_completed = false;
            for (const auto& gateway_message : messages_) {
                if (std::holds_alternative<protocol::ErrorMessage>(gateway_message)) {
                    const auto& error_message = std::get<protocol::ErrorMessage>(gateway_message);
                    saw_error = error_message.turn_id.has_value() &&
                                *error_message.turn_id == turn_id && error_message.code == code &&
                                error_message.message == message;
                }
                if (std::holds_alternative<protocol::TurnCompletedMessage>(gateway_message)) {
                    saw_turn_completed =
                        std::get<protocol::TurnCompletedMessage>(gateway_message).turn_id ==
                        turn_id;
                }
            }
            return saw_error && saw_turn_completed;
        });
    }

    std::optional<absl::Status> transport_closed_status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return transport_closed_status_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<protocol::GatewayMessage> messages_;
    std::optional<absl::Status> transport_closed_status_;
};

class AiGatewayClientSessionIntegrationTest : public ::testing::Test {
  protected:
    AiGatewayClientSessionIntegrationTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 0ms,
              .openai_client = MakeEchoOpenAiResponsesClient(),
          }),
          server_(
              GatewayServerConfig{
                  .bind_host = "127.0.0.1",
                  .port = 0,
                  .listen_backlog = 4,
              },
              &responder_, std::make_unique<SequentialSessionIdGenerator>("cli_test_")) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    void SetUp() override {
        ASSERT_TRUE(server_.Start().ok());
        ASSERT_NE(server_.bound_port(), 0);
    }

    void TearDown() override {
        server_.Stop();
    }

    GatewayStubResponder responder_;
    GatewayServer server_;
};

class AiGatewayClientSessionFailingProviderIntegrationTest : public ::testing::Test {
  protected:
    AiGatewayClientSessionFailingProviderIntegrationTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 0ms,
              .openai_client = MakeFailingOpenAiResponsesClient(),
          }),
          server_(
              GatewayServerConfig{
                  .bind_host = "127.0.0.1",
                  .port = 0,
                  .listen_backlog = 4,
              },
              &responder_, std::make_unique<SequentialSessionIdGenerator>("cli_fail_")) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    void SetUp() override {
        ASSERT_TRUE(server_.Start().ok());
        ASSERT_NE(server_.bound_port(), 0);
    }

    void TearDown() override {
        server_.Stop();
    }

    GatewayStubResponder responder_;
    GatewayServer server_;
};

TEST(AiGatewayClientSessionTest, RejectsHandshakeFailureCleanly) {
    RawTcpHandshakeRejectServer reject_server;
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = reject_server.port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_transport_closed =
            [&events](absl::Status status) { events.RecordTransportClosed(std::move(status)); },
    });

    const absl::Status status = session.ConnectAndStart();

    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(session.is_open());
    EXPECT_FALSE(session.session_id().has_value());
}

TEST(AiGatewayClientSessionTest, RejectsOperationsOutsideStartedSessionState) {
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = 8080,
        .path = "/",
        .operation_timeout = 2s,
    });

    const absl::Status send_before_start = session.SendTextInput("turn_1", "hello");
    EXPECT_FALSE(send_before_start.ok());
    EXPECT_EQ(send_before_start.code(), absl::StatusCode::kFailedPrecondition);

    const absl::Status cancel_before_start = session.RequestTurnCancel("turn_1");
    EXPECT_FALSE(cancel_before_start.ok());
    EXPECT_EQ(cancel_before_start.code(), absl::StatusCode::kFailedPrecondition);

    const absl::Status end_before_start = session.EndSession();
    EXPECT_FALSE(end_before_start.ok());
    EXPECT_EQ(end_before_start.code(), absl::StatusCode::kFailedPrecondition);

    session.Close();

    const absl::Status send_after_close = session.SendTextInput("turn_1", "hello");
    EXPECT_FALSE(send_after_close.ok());
    EXPECT_EQ(send_after_close.code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(AiGatewayClientSessionIntegrationTest, ConnectsSendsTurnReceivesReplyAndEndsSession) {
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&events](const protocol::GatewayMessage& message) { events.RecordMessage(message); },
        .on_transport_closed =
            [&events](absl::Status status) { events.RecordTransportClosed(std::move(status)); },
    });

    ASSERT_TRUE(session.ConnectAndStart().ok());
    ASSERT_TRUE(session.session_id().has_value());
    EXPECT_EQ(*session.session_id(), "cli_test_1");

    ASSERT_TRUE(session.SendTextInput("turn_1", "hello gateway").ok());
    ASSERT_TRUE(events.WaitForTextOutputAndCompletion("turn_1", "stub echo: hello gateway"));

    ASSERT_TRUE(session.EndSession().ok());
    ASSERT_TRUE(events.WaitForSessionEnded("cli_test_1"));

    session.Close();
}

TEST_F(AiGatewayClientSessionIntegrationTest, RequestsCancellationAndReceivesTurnCancelled) {
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&events](const protocol::GatewayMessage& message) { events.RecordMessage(message); },
    });

    ASSERT_TRUE(session.ConnectAndStart().ok());
    ASSERT_TRUE(session.SendTextInput("turn_cancel", "cancel me").ok());
    ASSERT_TRUE(session.RequestTurnCancel("turn_cancel").ok());

    ASSERT_TRUE(events.WaitForTurnCancelled("turn_cancel"));

    session.Close();
}

TEST_F(AiGatewayClientSessionFailingProviderIntegrationTest,
       SurfacesGatewayErrorFrameForAcceptedTurnFailure) {
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&events](const protocol::GatewayMessage& message) { events.RecordMessage(message); },
    });

    ASSERT_TRUE(session.ConnectAndStart().ok());
    ASSERT_TRUE(session.SendTextInput("turn_error", "hello gateway").ok());
    ASSERT_TRUE(events.WaitForErrorAndCompletion("turn_error", "service_unavailable",
                                                 "upstream service unavailable"));

    session.Close();
}

} // namespace
} // namespace isla::client
