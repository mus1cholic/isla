#include "isla/server/memory/supabase_memory_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#endif

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/log/log_sink_registry.h"
#include "ai_gateway_log_test_utils.hpp"

namespace isla::server::memory {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using nlohmann::json;
using namespace std::chrono_literals;

void ReportTestServerThreadException(std::string_view server_name) {
    try {
        throw;
    } catch (const std::exception& error) {
        ADD_FAILURE() << server_name << " worker thread threw exception: " << error.what();
    } catch (...) {
        ADD_FAILURE() << server_name << " worker thread threw a non-std exception";
    }
}

#if !defined(_WIN32)
namespace ssl = asio::ssl;

constexpr char kTestTlsServerKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
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
zNwzC+QhFTZoOomFoqMgFWujng==
-----END PRIVATE KEY-----)";

constexpr char kTestTlsServerCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIEWTCCAsGgAwIBAgIJAJinz4jHSjLtMA0GCSqGSIb3DQEBCwUAMF8xCzAJBgNV
BAYTAlhZMRcwFQYDVQQHDA5DYXN0bGUgQW50aHJheDEjMCEGA1UECgwaUHl0aG9u
IFNvZnR3YXJlIEZvdW5kYXRpb24xEjAQBgNVBAMMCWxvY2FsaG9zdDAeFw0xODA4
MjkxNDIzMTVaFw0yODA4MjYxNDIzMTVaMF8xCzAJBgNVBAYTAlhZMRcwFQYDVQQH
DA5DYXN0bGUgQW50aHJheDEjMCEGA1UECgwaUHl0aG9uIFNvZnR3YXJlIEZvdW5k
YXRpb24xEjAQBgNVBAMMCWxvY2FsaG9zdDCCAaIwDQYJKoZIhvcNAQEBBQADggGP
ADCCAYoCggGBALKUqUtopT6E68kN+uJNEt34i2EbmG/bwjcD8IaMsgJPSsMO2Bpd
3S6qWgkCeOyCfmAwBxK2kNbxGb63ouysEv7l8GCTJTWv3hG/HQcejJpnAEGi6K1U
fDbyE/db6yZ12SoHVTGkadN4vYGCPd1Wj9ZO1F877SHQ8rDWX3xgTWkxN2ojBw44
T8RHSDiG8D/CvG4uEy+VUszL+Uvny5y2poNSqvI3J56sptWSrh8nIIbkPZPBdUne
LYMOHTFK3ZjXSmhlXgziTxK71nnzM3Y9K9gxPnRqoXbvu/wFo55hQCkETiRkYgmm
jXcBMZ0TClQVnQWuLjMthRnWFZs4Lfmwqjs7FZD/61581R2BYehvpWbLvvuOJhwv
DFzexL2sXcAl7SsxbzeQKRHqGbIDfbnQTXfs3/VC6Ye5P82P2ucj+XC32N9piRmO
gCBP8L3ub+YzzdxikZN2gZXXE2jsb3QyE/R2LkWdWyshpKe+RsZP1SBRbHShUyOh
yJ90baoiEwj2mwIDAQABoxgwFjAUBgNVHREEDTALgglsb2NhbGhvc3QwDQYJKoZI
hvcNAQELBQADggGBAHRUO/UIHl3jXQENewYayHxkIx8t7nu40iO2DXbicSijz5bo
5//xAB6RxhBAlsDBehgQP1uoZg+WJW+nHu3CIVOU3qZNZRaozxiCl2UFKcNqLOmx
R3NKpo1jYf4REQIeG8Yw9+hSWLRbshNteP6bKUUf+vanhg9+axyOEOH/iOQvgk/m
b8wA8wNa4ujWljPbTQnj7ry8RqhTM0GcAN5LSdSvcKcpzLcs3aYwh+Z8e30sQWna
F40sa5u7izgBTOrwpcDm/w5kC46vpRQ5fnbshVw6pne2by0mdMECASid/p25N103
jMqTFlmO7kpf/jpCSmamp3/JSEE1BJKHwQ6Ql4nzRA2N1mnvWH7Zxcv043gkHeAu
0x8evpvwuhdIyproejNFlBpKmW8OX7yKTCPPMC/VkX8Q1rVkxU0DQ6hmvwZlhoKa
9Wc2uXpw9xF8itV4Uvcdr3dwqByvIqn7iI/gB+4l41e0u8OmH2MKOx4Nxlly5TNW
HcVKQHyOeyvnINuBAQ==
-----END CERTIFICATE-----)";
#endif

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

class SequentialHttpServer {
  public:
    explicit SequentialHttpServer(std::vector<std::string> responses)
        : responses_(std::move(responses)), acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~SequentialHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForRequestCount(std::size_t expected_count) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requests_.size() >= expected_count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    void Stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        boost::system::error_code error;
        acceptor_.close(error);
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            for (const std::string& response : responses_) {
                tcp::socket socket(io_context_);
                acceptor_.accept(socket);

                asio::streambuf buffer;
                asio::read_until(socket, buffer, "\r\n\r\n");
                std::string request;
                {
                    std::istream request_stream(&buffer);
                    request.assign(std::istreambuf_iterator<char>(request_stream),
                                   std::istreambuf_iterator<char>());
                }

                std::size_t content_length = 0U;
                const std::size_t header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::string headers = request.substr(0, header_end);
                    const std::string content_length_prefix = "Content-Length:";
                    const std::size_t content_length_pos = headers.find(content_length_prefix);
                    if (content_length_pos != std::string::npos) {
                        const std::size_t value_begin =
                            content_length_pos + content_length_prefix.size();
                        const std::size_t value_end = headers.find("\r\n", value_begin);
                        content_length = static_cast<std::size_t>(
                            std::stoul(headers.substr(value_begin, value_end - value_begin)));
                    }
                }

                const std::size_t body_already_buffered =
                    header_end == std::string::npos ? 0U : request.size() - (header_end + 4U);
                if (body_already_buffered < content_length) {
                    const std::size_t remaining = content_length - body_already_buffered;
                    std::string tail(remaining, '\0');
                    asio::read(socket, asio::buffer(tail.data(), tail.size()));
                    request += tail;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                }

                asio::write(socket, asio::buffer(response.data(), response.size()));
                boost::system::error_code error;
                socket.shutdown(tcp::socket::shutdown_both, error);
                socket.close(error);
            }
        } catch (...) {
            ReportTestServerThreadException("SequentialHttpServer");
        }
    }

    std::vector<std::string> responses_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::uint16_t port_ = 0;
};

class RoutingHttpServer {
  public:
    explicit RoutingHttpServer(std::unordered_map<std::string, std::string> responses_by_target,
                               std::size_t expected_request_count = 0U)
        : responses_by_target_(std::move(responses_by_target)),
          expected_request_count_(expected_request_count == 0U ? responses_by_target_.size()
                                                               : expected_request_count),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~RoutingHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForRequestCount(std::size_t expected_count) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requests_.size() >= expected_count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    void Stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        boost::system::error_code error;
        acceptor_.close(error);
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            while (!stopped_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (requests_.size() >= expected_request_count_) {
                        break;
                    }
                }
                tcp::socket socket(io_context_);
                boost::system::error_code accept_error;
                acceptor_.accept(socket, accept_error);
                if (accept_error) {
                    if (stopped_.load()) {
                        break;
                    }
                    throw std::runtime_error("RoutingHttpServer accept failed: " +
                                             accept_error.message());
                }

                asio::streambuf buffer;
                asio::read_until(socket, buffer, "\r\n\r\n");
                std::string request;
                {
                    std::istream request_stream(&buffer);
                    request.assign(std::istreambuf_iterator<char>(request_stream),
                                   std::istreambuf_iterator<char>());
                }

                const std::size_t first_line_end = request.find("\r\n");
                const std::string request_line = first_line_end == std::string::npos
                                                     ? request
                                                     : request.substr(0, first_line_end);

                std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request_line);
                    for (const auto& [target_path, candidate_response] : responses_by_target_) {
                        if (request_line.find(target_path) != std::string::npos) {
                            response = candidate_response;
                            break;
                        }
                    }
                }

                asio::write(socket, asio::buffer(response.data(), response.size()));
                boost::system::error_code error;
                socket.shutdown(tcp::socket::shutdown_both, error);
                socket.close(error);
            }
        } catch (...) {
            ReportTestServerThreadException("RoutingHttpServer");
        }
    }

    std::unordered_map<std::string, std::string> responses_by_target_;
    std::size_t expected_request_count_ = 0U;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::uint16_t port_ = 0;
};

#if !defined(_WIN32)
class OneShotHttpsServer {
  public:
    explicit OneShotHttpsServer(std::string response)
        : response_(std::move(response)), ssl_context_(ssl::context::tls_server),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        boost::system::error_code error;
        ssl_context_.use_certificate_chain(asio::buffer(kTestTlsServerCertPem), error);
        if (error) {
            throw std::runtime_error("failed to configure test TLS certificate chain");
        }
        ssl_context_.use_private_key(asio::buffer(kTestTlsServerKeyPem), ssl::context::pem, error);
        if (error) {
            throw std::runtime_error("failed to configure test TLS private key");
        }
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~OneShotHttpsServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForRequest() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (request_text_.has_value()) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

  private:
    void Stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        boost::system::error_code error;
        acceptor_.close(error);
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            tcp::socket socket(io_context_);
            acceptor_.accept(socket);

            ssl::stream<tcp::socket> ssl_stream(std::move(socket), ssl_context_);
            ssl_stream.handshake(ssl::stream_base::server);

            asio::streambuf buffer;
            asio::read_until(ssl_stream, buffer, "\r\n\r\n");
            std::string request;
            {
                std::istream request_stream(&buffer);
                request.assign(std::istreambuf_iterator<char>(request_stream),
                               std::istreambuf_iterator<char>());
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_text_ = request;
            }

            asio::write(ssl_stream, asio::buffer(response_.data(), response_.size()));
            boost::system::error_code ignored;
            ssl_stream.shutdown(ignored);
        } catch (...) {
            ReportTestServerThreadException("OneShotHttpsServer");
        }
    }

    std::string response_;
    mutable std::mutex mutex_;
    std::optional<std::string> request_text_;
    asio::io_context io_context_;
    ssl::context ssl_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::uint16_t port_ = 0;
};
#endif

TEST(SupabaseMemoryStoreTest, CreateRejectsInvalidUrl) {
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "not-a-url",
            .service_role_key = "key",
        });

    ASSERT_FALSE(store.ok());
    EXPECT_EQ(store.status().code(), absl::StatusCode::kInvalidArgument);
}

#if defined(_WIN32)
TEST(SupabaseMemoryStoreTest, CreateRejectsHttpsOnWindows) {
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "https://project.supabase.co",
            .service_role_key = "service_role_key",
        });

    ASSERT_FALSE(store.ok());
    EXPECT_EQ(store.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_NE(store.status().message().find("Windows builds"), std::string::npos);
}
#endif

TEST(SupabaseMemoryStoreTest, AppendConversationMessageCreatesConversationItemThenMessage) {
    SequentialHttpServer server({
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n",
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::Status status = (*store)->AppendConversationMessage(ConversationMessageWrite{
        .session_id = "session_001",
        .conversation_item_index = 0,
        .message_index = 0,
        .turn_id = "turn_001",
        .role = MessageRole::User,
        .content = "hello",
        .create_time = Ts("2026-03-08T14:00:00Z"),
    });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequestCount(2U));
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_NE(requests[0].find(
                  "POST /rest/v1/conversation_items?on_conflict=session_id%2Citem_index HTTP/1.1"),
              std::string::npos);
    EXPECT_NE(requests[0].find("Content-Profile: public"), std::string::npos);
    EXPECT_NE(
        requests[1].find(
            "POST "
            "/rest/v1/conversation_messages?on_conflict=session_id%2Citem_index%2Cmessage_index "
            "HTTP/1.1"),
        std::string::npos);

    const std::size_t body_pos = requests[1].find("\r\n\r\n");
    ASSERT_NE(body_pos, std::string::npos);
    const json body = json::parse(requests[1].substr(body_pos + 4U));
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body[0]["session_id"], "session_001");
    EXPECT_EQ(body[0]["item_index"], 0);
    EXPECT_EQ(body[0]["message_index"], 0);
    EXPECT_EQ(body[0]["turn_id"], "turn_001");
    EXPECT_EQ(body[0]["content"], "hello");
}

TEST(SupabaseMemoryStoreTest,
     SplitConversationItemWithEpisodeStubInvokesSupabaseRpcWithRemainingMessages) {
    SequentialHttpServer server({
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::Status status = (*store)->SplitConversationItemWithEpisodeStub(SplitEpisodeStubWrite{
        .session_id = "session_001",
        .conversation_item_index = 0,
        .episode_id = "ep_001",
        .episode_stub_content = "first exchange ref",
        .episode_stub_create_time = Ts("2026-03-08T14:00:04Z"),
        .remaining_ongoing_episode =
            OngoingEpisode{
                .messages =
                    {
                        Message{
                            .role = MessageRole::User,
                            .content = "follow up",
                            .create_time = Ts("2026-03-08T14:00:02Z"),
                        },
                        Message{
                            .role = MessageRole::Assistant,
                            .content = "here is more help",
                            .create_time = Ts("2026-03-08T14:00:03Z"),
                        },
                    },
            },
    });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequestCount(1U));
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(
        requests[0].find("POST /rest/v1/rpc/split_conversation_item_with_episode_stub HTTP/1.1"),
        std::string::npos);
    EXPECT_NE(requests[0].find("Content-Profile: public"), std::string::npos);

    const std::size_t body_pos = requests[0].find("\r\n\r\n");
    ASSERT_NE(body_pos, std::string::npos);
    const json body = json::parse(requests[0].substr(body_pos + 4U));
    EXPECT_EQ(body["p_session_id"], "session_001");
    EXPECT_EQ(body["p_conversation_item_index"], 0);
    EXPECT_EQ(body["p_episode_id"], "ep_001");
    EXPECT_EQ(body["p_episode_stub_content"], "first exchange ref");
    EXPECT_EQ(body["p_episode_stub_created_at"], "2026-03-08T14:00:04Z");
    ASSERT_TRUE(body["p_remaining_messages"].is_array());
    ASSERT_EQ(body["p_remaining_messages"].size(), 2U);
    EXPECT_EQ(body["p_remaining_messages"][0]["role"], "user");
    EXPECT_EQ(body["p_remaining_messages"][0]["content"], "follow up");
    EXPECT_EQ(body["p_remaining_messages"][0]["created_at"], "2026-03-08T14:00:02Z");
    EXPECT_EQ(body["p_remaining_messages"][1]["role"], "assistant");
    EXPECT_EQ(body["p_remaining_messages"][1]["content"], "here is more help");
    EXPECT_EQ(body["p_remaining_messages"][1]["created_at"], "2026-03-08T14:00:03Z");
}

TEST(SupabaseMemoryStoreTest,
     SplitConversationItemWithEpisodeStubPropagatesFailedPreconditionFromRpc) {
    const std::string failure_body =
        R"({"message":"remaining_ongoing_episode does not match persisted conversation messages"})";
    SequentialHttpServer server({
        "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(failure_body.size()) + "\r\n\r\n" + failure_body,
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::Status status = (*store)->SplitConversationItemWithEpisodeStub(SplitEpisodeStubWrite{
        .session_id = "session_001",
        .conversation_item_index = 0,
        .episode_id = "ep_001",
        .episode_stub_content = "first exchange ref",
        .episode_stub_create_time = Ts("2026-03-08T14:00:04Z"),
        .remaining_ongoing_episode =
            OngoingEpisode{
                .messages =
                    {
                        Message{
                            .role = MessageRole::User,
                            .content = "follow up",
                            .create_time = Ts("2026-03-08T14:00:02Z"),
                        },
                    },
            },
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_NE(status.message().find("does not match"), std::string::npos);
    ASSERT_TRUE(server.WaitForRequestCount(1U));
}

TEST(SupabaseMemoryStoreTest, SplitConversationItemWithEpisodeStubPropagatesRpcFailure) {
    const std::string failure_body = R"({"message":"supabase unavailable"})";
    SequentialHttpServer server({
        "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(failure_body.size()) + "\r\n\r\n" + failure_body,
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::Status status = (*store)->SplitConversationItemWithEpisodeStub(SplitEpisodeStubWrite{
        .session_id = "session_001",
        .conversation_item_index = 0,
        .episode_id = "ep_001",
        .episode_stub_content = "first exchange ref",
        .episode_stub_create_time = Ts("2026-03-08T14:00:04Z"),
        .remaining_ongoing_episode =
            OngoingEpisode{
                .messages =
                    {
                        Message{
                            .role = MessageRole::User,
                            .content = "follow up",
                            .create_time = Ts("2026-03-08T14:00:02Z"),
                        },
                    },
            },
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_NE(status.message().find("supabase unavailable"), std::string::npos);
    ASSERT_TRUE(server.WaitForRequestCount(1U));
}

TEST(SupabaseMemoryStoreTest, LogsRequestAndOperationLatencyWhenTelemetryEnabled) {
    SequentialHttpServer server({
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n",
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .telemetry_logging_enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    isla::server::test::CapturingLogSink log_sink;
    absl::AddLogSink(&log_sink);

    const absl::Status status = (*store)->UpsertSession(MemorySessionRecord{
        .session_id = "session_telemetry",
        .user_id = "user_001",
        .system_prompt = "You are Isla.",
        .created_at = Ts("2026-03-08T14:00:00Z"),
        .ended_at = std::nullopt,
    });

    absl::RemoveLogSink(&log_sink);

    ASSERT_TRUE(status.ok()) << status;
    EXPECT_TRUE(log_sink.Contains("AI gateway supabase latency kind=request"));
    EXPECT_TRUE(log_sink.Contains("target=/rest/v1/memory_sessions"));
    EXPECT_TRUE(log_sink.Contains("AI gateway supabase latency kind=operation"));
    EXPECT_TRUE(log_sink.Contains("op=upsert_session"));
    EXPECT_TRUE(log_sink.Contains("session_id=session_telemetry"));
}

TEST(SupabaseMemoryStoreTest, ListMidTermEpisodesReturnsOrderedEpisodesForSession) {
    const std::string episodes_body =
        "[{\"episode_id\":\"ep_older\",\"tier1_detail\":null,\"tier2_summary\":\"older "
        "summary\",\"tier3_ref\":\"older ref\",\"tier3_keywords\":[\"older\"],\"salience\":3,"
        "\"embedding\":[],\"created_at\":\"2026-03-08T14:00:00Z\"},"
        "{\"episode_id\":\"ep_newer\",\"tier1_detail\":\"full detail\",\"tier2_summary\":\"newer "
        "summary\",\"tier3_ref\":\"newer ref\",\"tier3_keywords\":[\"newer\"],\"salience\":9,"
        "\"embedding\":[],\"created_at\":\"2026-03-08T14:01:00Z\"}]";
    RoutingHttpServer server({
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episodes_body.size()) + "\r\n\r\n" + episodes_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::vector<Episode>> episodes =
        (*store)->ListMidTermEpisodes("session_001");

    ASSERT_TRUE(episodes.ok()) << episodes.status();
    ASSERT_TRUE(server.WaitForRequestCount(1U));
    ASSERT_EQ(episodes->size(), 2U);
    EXPECT_EQ((*episodes)[0].episode_id, "ep_older");
    EXPECT_EQ((*episodes)[1].episode_id, "ep_newer");
    EXPECT_TRUE((*episodes)[1].tier1_detail.has_value());
}

TEST(SupabaseMemoryStoreTest, ListMidTermEpisodesRejectsEmptySessionId) {
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:1",
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::vector<Episode>> episodes = (*store)->ListMidTermEpisodes("");

    ASSERT_FALSE(episodes.ok());
    EXPECT_EQ(episodes.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SupabaseMemoryStoreTest, ListMidTermEpisodesRejectsMalformedEpisodeRows) {
    const std::string episodes_body =
        "[{\"episode_id\":\"ep_001\",\"tier1_detail\":null,\"tier3_ref\":\"ref\","
        "\"tier3_keywords\":[\"memory\"],\"salience\":8,\"embedding\":[],"
        "\"created_at\":\"2026-03-08T14:00:02Z\"}]";
    RoutingHttpServer server({
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episodes_body.size()) + "\r\n\r\n" + episodes_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::vector<Episode>> episodes =
        (*store)->ListMidTermEpisodes("session_001");

    ASSERT_FALSE(episodes.ok());
    EXPECT_EQ(episodes.status().code(), absl::StatusCode::kInternal);
    ASSERT_TRUE(server.WaitForRequestCount(1U));
}

TEST(SupabaseMemoryStoreTest, GetMidTermEpisodeReturnsSingleSessionScopedEpisode) {
    const std::string episode_body =
        "[{\"episode_id\":\"ep_001\",\"tier1_detail\":\"full detail\",\"tier2_summary\":"
        "\"summary\",\"tier3_ref\":\"ref\",\"tier3_keywords\":[\"memory\"],\"salience\":8,"
        "\"embedding\":[],\"created_at\":\"2026-03-08T14:00:02Z\"}]";
    RoutingHttpServer server({
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episode_body.size()) + "\r\n\r\n" + episode_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::optional<Episode>> episode =
        (*store)->GetMidTermEpisode("session_001", "ep_001");

    ASSERT_TRUE(episode.ok()) << episode.status();
    ASSERT_TRUE(server.WaitForRequestCount(1U));
    ASSERT_TRUE(episode->has_value());
    EXPECT_EQ(episode->value().episode_id, "ep_001");
    EXPECT_EQ(episode->value().tier1_detail, std::optional<std::string>("full detail"));
}

TEST(SupabaseMemoryStoreTest, GetMidTermEpisodeReturnsNulloptWhenNoRowsMatch) {
    const std::string episode_body = "[]";
    RoutingHttpServer server({
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episode_body.size()) + "\r\n\r\n" + episode_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::optional<Episode>> episode =
        (*store)->GetMidTermEpisode("session_001", "ep_missing");

    ASSERT_TRUE(episode.ok()) << episode.status();
    ASSERT_TRUE(server.WaitForRequestCount(1U));
    EXPECT_FALSE(episode->has_value());
}

TEST(SupabaseMemoryStoreTest, GetMidTermEpisodeRejectsMissingIdentifiers) {
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:1",
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::optional<Episode>> missing_session =
        (*store)->GetMidTermEpisode("", "ep_001");
    ASSERT_FALSE(missing_session.ok());
    EXPECT_EQ(missing_session.status().code(), absl::StatusCode::kInvalidArgument);

    const absl::StatusOr<std::optional<Episode>> missing_episode =
        (*store)->GetMidTermEpisode("session_001", "");
    ASSERT_FALSE(missing_episode.ok());
    EXPECT_EQ(missing_episode.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SupabaseMemoryStoreTest, GetMidTermEpisodeRejectsMalformedEpisodeRows) {
    const std::string episode_body =
        "[{\"episode_id\":\"ep_001\",\"tier1_detail\":null,\"tier3_ref\":\"ref\","
        "\"tier3_keywords\":[\"memory\"],\"salience\":8,\"embedding\":[],"
        "\"created_at\":\"2026-03-08T14:00:02Z\"}]";
    RoutingHttpServer server({
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episode_body.size()) + "\r\n\r\n" + episode_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::optional<Episode>> episode =
        (*store)->GetMidTermEpisode("session_001", "ep_001");

    ASSERT_FALSE(episode.ok());
    EXPECT_EQ(episode.status().code(), absl::StatusCode::kInternal);
    ASSERT_TRUE(server.WaitForRequestCount(1U));
}

TEST(SupabaseMemoryStoreTest, LoadSnapshotHydratesConversationAndMidTermEpisodes) {
    const std::string session_body =
        "[{\"session_id\":\"session_001\",\"user_id\":\"user_001\",\"system_prompt\":\"You are "
        "Isla.\",\"created_at\":\"2026-03-08T14:00:00Z\",\"ended_at\":null}]";
    const std::string items_body =
        "[{\"item_index\":0,\"item_type\":\"episode_stub\",\"episode_id\":\"ep_001\",\"episode_"
        "stub_content\":\"summary "
        "ref\",\"episode_stub_created_at\":\"2026-03-08T14:00:03Z\"},{\"item_index\":1,\"item_"
        "type\":\"ongoing_episode\",\"episode_id\":null,\"episode_stub_content\":null,\"episode_"
        "stub_created_at\":null}]";
    const std::string messages_body =
        "[{\"item_index\":0,\"message_index\":0,\"role\":\"user\",\"content\":\"hello\","
        "\"created_at\":\"2026-03-08T14:00:00Z\"},"
        "{\"item_index\":0,\"message_index\":1,\"role\":\"assistant\",\"content\":\"hi there\","
        "\"created_at\":\"2026-03-08T14:00:01Z\"},"
        "{\"item_index\":1,\"message_index\":0,\"role\":\"user\",\"content\":\"follow "
        "up\",\"created_at\":\"2026-03-08T14:00:04Z\"}]";
    const std::string episodes_body = "[{\"episode_id\":\"ep_001\",\"tier1_detail\":null,\"tier2_"
                                      "summary\":\"summary\",\"tier3_ref\":\"summary "
                                      "ref\",\"tier3_keywords\":[\"memory\"],\"salience\":7,"
                                      "\"embedding\":[],\"created_at\":\"2026-03-08T14:00:02Z\"}]";
    RoutingHttpServer server({
        { "/rest/v1/memory_sessions",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(session_body.size()) + "\r\n\r\n" + session_body },
        { "/rest/v1/conversation_items",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(items_body.size()) + "\r\n\r\n" + items_body },
        { "/rest/v1/conversation_messages",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(messages_body.size()) + "\r\n\r\n" + messages_body },
        { "/rest/v1/mid_term_episodes",
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(episodes_body.size()) + "\r\n\r\n" + episodes_body },
    });
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "http://127.0.0.1:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::StatusOr<std::optional<MemoryStoreSnapshot>> snapshot =
        (*store)->LoadSnapshot("session_001");

    ASSERT_TRUE(snapshot.ok()) << snapshot.status();
    ASSERT_TRUE(snapshot->has_value());
    ASSERT_TRUE(server.WaitForRequestCount(4U));
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 4U);
    EXPECT_NE(
        requests[2].find("/rest/v1/conversation_messages?select=item_index%2Cmessage_index%2Crole"
                         "%2Ccontent%2Ccreated_at%2Cconversation_items%21inner%28item_type%29"),
        std::string::npos);
    EXPECT_NE(requests[2].find("conversation_items.item_type=eq.ongoing_episode"),
              std::string::npos);
    ASSERT_EQ(snapshot->value().conversation_items.size(), 2U);
    EXPECT_EQ(snapshot->value().conversation_items[0].type, ConversationItemType::EpisodeStub);
    EXPECT_EQ(snapshot->value().conversation_items[0].episode_stub->content, "summary ref");
    EXPECT_EQ(snapshot->value().conversation_items[1].type, ConversationItemType::OngoingEpisode);
    EXPECT_FALSE(snapshot->value().conversation_items[0].ongoing_episode.has_value());
    ASSERT_TRUE(snapshot->value().conversation_items[1].ongoing_episode.has_value());
    ASSERT_EQ(snapshot->value().conversation_items[1].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(snapshot->value().conversation_items[1].ongoing_episode->messages[0].content,
              "follow up");
    ASSERT_EQ(snapshot->value().mid_term_episodes.size(), 1U);
    EXPECT_EQ(snapshot->value().mid_term_episodes[0].episode_id, "ep_001");
}

#if !defined(_WIN32)
TEST(SupabaseMemoryStoreTest, UpsertSessionUsesHttpsWithInjectedTrust) {
    OneShotHttpsServer server("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    const absl::StatusOr<MemoryStorePtr> store =
        CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig{
            .enabled = true,
            .url = "https://localhost:" + std::to_string(server.port()),
            .service_role_key = "service_role_key",
            .schema = "public",
            .request_timeout = 2s,
            .trusted_ca_cert_pem = std::string(kTestTlsServerCertPem),
        });
    ASSERT_TRUE(store.ok()) << store.status();

    const absl::Status status = (*store)->UpsertSession(MemorySessionRecord{
        .session_id = "session_001",
        .user_id = "user_001",
        .system_prompt = "You are Isla.",
        .created_at = Ts("2026-03-08T14:00:00Z"),
        .ended_at = std::nullopt,
    });

    ASSERT_TRUE(status.ok()) << status;
    EXPECT_TRUE(server.WaitForRequest());
}
#endif

} // namespace
} // namespace isla::server::memory
