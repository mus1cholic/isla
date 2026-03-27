#include "ai_gateway_client_session.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
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
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/server/openai_responses_client.hpp"
#include "server/src/openai_responses_test_utils.hpp"

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
using isla::server::ai_gateway::test::ExtractLatestPromptLine;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::ConversationMessageWrite;
using isla::server::memory::Episode;
using isla::server::memory::MemorySessionRecord;
using isla::server::memory::MemoryStore;
using isla::server::memory::MemoryStoreSnapshot;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::string_view kTestUserId = "client_user";

class RecordingMemoryStore final : public MemoryStore {
  public:
    absl::Status UpsertSession(const MemorySessionRecord& record) override {
        session_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(const ConversationMessageWrite& write) override {
        message_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& /*write*/) override {
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& /*write*/) override {
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& /*write*/) override {
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view /*session_id*/) const override {
        return std::vector<Episode>{};
    }

    absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view /*session_id*/,
                      std::string_view /*episode_id*/) const override {
        return std::nullopt;
    }

    absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view /*session_id*/) const override {
        return std::nullopt;
    }

    std::vector<MemorySessionRecord> session_records;
    std::vector<ConversationMessageWrite> message_writes;
};

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = "stub echo: " + isla::server::ai_gateway::test::ExtractLatestPromptLine(
                                              request.user_text),
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

absl::Status EmitResponseText(std::string_view text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id = "resp_client_test") {
    absl::Status delta_status =
        on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = std::string(text) });
    if (!delta_status.ok()) {
        return delta_status;
    }
    return on_event(OpenAiResponsesCompletedEvent{
        .response_id = std::string(response_id),
    });
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
                    saw_text_output |= text_output.turn_id == turn_id && text_output.text == text;
                }
                if (std::holds_alternative<protocol::TurnCompletedMessage>(message)) {
                    saw_turn_completed |=
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

    bool WaitForTranscriptSeeded(std::string_view turn_id, std::string_view role) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] {
            for (const auto& message : messages_) {
                if (std::holds_alternative<protocol::TranscriptSeededMessage>(message)) {
                    const auto& seeded = std::get<protocol::TranscriptSeededMessage>(message);
                    if (seeded.turn_id == turn_id && seeded.role == role) {
                        return true;
                    }
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

    bool WaitForTransportClosed() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] { return transport_closed_status_.has_value(); });
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
                    saw_error |= error_message.turn_id.has_value() &&
                                 *error_message.turn_id == turn_id && error_message.code == code &&
                                 error_message.message == message;
                }
                if (std::holds_alternative<protocol::TurnCompletedMessage>(gateway_message)) {
                    saw_turn_completed |=
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
              .response_delay = 100ms,
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

    const absl::Status status = session.ConnectAndStart(std::string(kTestUserId));

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

    const absl::Status seed_before_start =
        session.SendTranscriptSeed("turn_1", "assistant", "seeded");
    EXPECT_FALSE(seed_before_start.ok());
    EXPECT_EQ(seed_before_start.code(), absl::StatusCode::kFailedPrecondition);

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

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session.session_id().has_value());
    EXPECT_EQ(*session.session_id(), "cli_test_1");

    ASSERT_TRUE(session.SendTextInput("turn_1", "hello gateway").ok());
    ASSERT_TRUE(events.WaitForTextOutputAndCompletion("turn_1", "stub echo: hello gateway"));

    ASSERT_TRUE(session.EndSession().ok());
    ASSERT_TRUE(events.WaitForSessionEnded("cli_test_1"));
    ASSERT_TRUE(events.WaitForTransportClosed());
    ASSERT_TRUE(events.transport_closed_status().has_value());
    EXPECT_TRUE(events.transport_closed_status()->ok())
        << events.transport_closed_status()->ToString();
    const absl::Status send_after_end = session.SendTextInput("turn_2", "after end");
    EXPECT_FALSE(send_after_end.ok());
    EXPECT_EQ(send_after_end.code(), absl::StatusCode::kFailedPrecondition);

    session.Close();
}

TEST_F(AiGatewayClientSessionIntegrationTest, EndSessionReportsCleanTransportClosure) {
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

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session.session_id().has_value());
    const std::string session_id = *session.session_id();

    ASSERT_TRUE(session.EndSession().ok());
    ASSERT_TRUE(events.WaitForSessionEnded(session_id));
    ASSERT_TRUE(events.WaitForTransportClosed());
    ASSERT_TRUE(events.transport_closed_status().has_value());
    EXPECT_TRUE(events.transport_closed_status()->ok())
        << events.transport_closed_status()->ToString();
    EXPECT_FALSE(session.is_open());

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

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session.SendTextInput("turn_cancel", "cancel me").ok());
    ASSERT_TRUE(session.RequestTurnCancel("turn_cancel").ok());

    ASSERT_TRUE(events.WaitForTurnCancelled("turn_cancel"));

    session.Close();
}

TEST_F(AiGatewayClientSessionIntegrationTest, SendsTranscriptSeedAndReceivesAcknowledgement) {
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&events](const protocol::GatewayMessage& message) { events.RecordMessage(message); },
    });

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session.SendTranscriptSeed("turn_seed", "assistant", "seeded context").ok());
    ASSERT_TRUE(events.WaitForTranscriptSeeded("turn_seed", "assistant"));

    session.Close();
}

TEST_F(AiGatewayClientSessionIntegrationTest, SendsReplayTimestampsThroughGateway) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_client_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.system_prompt.find("should_flush") != std::string::npos) {
                return EmitResponseText(R"json({
            "should_flush": false,
            "item_id": null,
            "split_at": null,
            "reasoning": "No completed episode boundary."
        })json",
                                        on_event, "resp_client_decider");
            }
            if (request.system_prompt.find("tier2_summary") != std::string::npos) {
                return EmitResponseText(R"json({
            "tier1_detail": "detail",
            "tier2_summary": "summary",
            "tier3_ref": "ref",
            "tier3_keywords": ["client", "timestamp", "memory", "test", "summary"],
            "salience": 5
        })json",
                                        on_event, "resp_client_compactor");
            }
            return EmitResponseText("stub echo: " + ExtractLatestPromptLine(request.user_text),
                                    on_event);
        });
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .memory_store = store,
        .openai_client = client,
    });
    GatewayServer server(
        GatewayServerConfig{
            .bind_host = "127.0.0.1",
            .port = 0,
            .listen_backlog = 4,
        },
        &responder, std::make_unique<SequentialSessionIdGenerator>("cli_ts_"));
    responder.AttachSessionRegistry(&server.session_registry());
    ASSERT_TRUE(server.Start().ok());

    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&events](const protocol::GatewayMessage& message) { events.RecordMessage(message); },
    });

    ASSERT_TRUE(
        session.ConnectAndStart(std::string(kTestUserId), "client_session_1",
                                "2026-03-14T09:59:00Z", "2026-03-20T08:00:00Z")
            .ok());
    ASSERT_TRUE(
        session
            .SendTranscriptSeed("turn_seed", "assistant", "seeded context", "2026-03-14T10:00:05Z")
            .ok());
    ASSERT_TRUE(events.WaitForTranscriptSeeded("turn_seed", "assistant"));
    ASSERT_TRUE(session.SendTextInput("turn_1", "hello gateway", "2026-03-15T11:30:00Z").ok());
    ASSERT_TRUE(events.WaitForTextOutputAndCompletion("turn_1", "stub echo: hello gateway"));

    ASSERT_EQ(store->session_records.size(), 1U);
    EXPECT_EQ(store->session_records[0].created_at,
              isla::server::memory::ParseTimestamp("2026-03-14T09:59:00Z"));
    ASSERT_GE(store->message_writes.size(), 3U);
    EXPECT_EQ(store->message_writes[0].create_time,
              isla::server::memory::ParseTimestamp("2026-03-14T10:00:05Z"));
    EXPECT_EQ(store->message_writes[1].create_time,
              isla::server::memory::ParseTimestamp("2026-03-15T11:30:00Z"));

    session.Close();
    server.Stop();
}

TEST_F(AiGatewayClientSessionIntegrationTest, RejectsSendAfterTransportFailureWithoutTimingOut) {
    RecordingClientEvents events;
    AiGatewayClientSession session(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_transport_closed =
            [&events](absl::Status status) { events.RecordTransportClosed(std::move(status)); },
    });

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    server_.Stop();
    ASSERT_TRUE(events.WaitForTransportClosed());

    const absl::Status send_after_failure = session.SendTextInput("turn_1", "hello");
    EXPECT_FALSE(send_after_failure.ok());
    EXPECT_EQ(send_after_failure.code(), absl::StatusCode::kFailedPrecondition);

    session.Close();
}

TEST_F(AiGatewayClientSessionIntegrationTest, CloseIsSafeWhenCalledFromTransportClosedCallback) {
    std::mutex mutex;
    std::condition_variable cv;
    bool close_called_from_callback = false;
    std::unique_ptr<AiGatewayClientSession> session;
    session = std::make_unique<AiGatewayClientSession>(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_transport_closed =
            [&mutex, &cv, &close_called_from_callback, &session](absl::Status /*status*/) {
                session->Close();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    close_called_from_callback = true;
                }
                cv.notify_all();
            },
    });

    ASSERT_TRUE(session->ConnectAndStart(std::string(kTestUserId)).ok());
    server_.Stop();

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return close_called_from_callback; }));
    }

    session.reset();
}

TEST_F(AiGatewayClientSessionIntegrationTest, CloseIsSafeWhenCalledFromOnMessageCallback) {
    std::mutex mutex;
    std::condition_variable cv;
    bool close_called_from_callback = false;
    bool transport_closed = false;
    std::unique_ptr<AiGatewayClientSession> session;
    session = std::make_unique<AiGatewayClientSession>(AiGatewayClientConfig{
        .host = "127.0.0.1",
        .port = server_.bound_port(),
        .path = "/",
        .operation_timeout = 2s,
        .on_message =
            [&mutex, &cv, &close_called_from_callback,
             &session](const protocol::GatewayMessage& message) {
                if (!std::holds_alternative<protocol::TextOutputMessage>(message)) {
                    return;
                }
                session->Close();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    close_called_from_callback = true;
                }
                cv.notify_all();
            },
        .on_transport_closed =
            [&mutex, &cv, &transport_closed](absl::Status /*status*/) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    transport_closed = true;
                }
                cv.notify_all();
            },
    });

    ASSERT_TRUE(session->ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session->SendTextInput("turn_close", "close me").ok());

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(
            cv.wait_for(lock, 2s, [&] { return close_called_from_callback && transport_closed; }));
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    absl::Status reconnect_status = absl::FailedPreconditionError("reconnect still pending");
    while (std::chrono::steady_clock::now() < deadline) {
        reconnect_status = session->ConnectAndStart(std::string(kTestUserId));
        if (reconnect_status.ok()) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    ASSERT_TRUE(reconnect_status.ok()) << reconnect_status;
    session->Close();
    session.reset();
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

    ASSERT_TRUE(session.ConnectAndStart(std::string(kTestUserId)).ok());
    ASSERT_TRUE(session.SendTextInput("turn_error", "hello gateway").ok());
    ASSERT_TRUE(events.WaitForErrorAndCompletion("turn_error", "service_unavailable",
                                                 "upstream service unavailable"));

    session.Close();
}

} // namespace
} // namespace isla::client
