#include "isla/server/openai_responses_client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

class OneShotHttpServer {
  public:
    explicit OneShotHttpServer(std::string response)
        : response_(std::move(response)), acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~OneShotHttpServer() {
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

    [[nodiscard]] std::string request_text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_text_.value_or("");
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

            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            std::string request;
            {
                std::istream request_stream(&buffer);
                request.assign(std::istreambuf_iterator<char>(request_stream),
                               std::istreambuf_iterator<char>());
            }

            std::size_t content_length = 0;
            const std::string_view header_terminator = "\r\n\r\n";
            const std::size_t header_end = request.find(header_terminator);
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
                request_text_ = request;
            }

            asio::write(socket, asio::buffer(response_.data(), response_.size()));
            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
        }
    }

    std::string response_;
    mutable std::mutex mutex_;
    std::optional<std::string> request_text_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::uint16_t port_ = 0;
};

TEST(OpenAiResponsesClientTest, StreamsSseDeltasAndCompletionOverHttp) {
    const std::string body =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello \"}\r\n\r\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"world\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    std::string output_text;
    int completed_count = 0;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "system prompt",
            .user_text = "hello",
        },
        [&](const OpenAiResponsesEvent& event) -> absl::Status {
            return std::visit(
                [&](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        output_text += concrete_event.text_delta;
                    } else if constexpr (std::is_same_v<Event, OpenAiResponsesCompletedEvent>) {
                        ++completed_count;
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequest());
    EXPECT_EQ(output_text, "hello world");
    EXPECT_EQ(completed_count, 1);
    EXPECT_NE(server.request_text().find("POST /v1/responses HTTP/1.1"), std::string::npos);
    EXPECT_NE(server.request_text().find("Authorization: Bearer test_key"), std::string::npos);
    EXPECT_NE(server.request_text().find("\"stream\":true"), std::string::npos);
    EXPECT_NE(server.request_text().find("\"model\":\"gpt-5.2\""), std::string::npos);
    EXPECT_NE(server.request_text().find("\"instructions\":\"system prompt\""), std::string::npos);
}

TEST(OpenAiResponsesClientTest, PassesHeaderValuesWithShellMetacharactersLiterally) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
        .organization = "org&unsafe|value%^name",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequest());
    EXPECT_NE(server.request_text().find("OpenAI-Organization: org&unsafe|value%^name"),
              std::string::npos);
}

TEST(OpenAiResponsesClientTest, ExtractsFinalTextFromCompletedEventWhenNoDeltaArrives) {
    const std::string body =
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"output\":["
        "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"final "
        "only\"}]}]}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    std::string output_text;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [&](const OpenAiResponsesEvent& event) -> absl::Status {
            return std::visit(
                [&](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        output_text += concrete_event.text_delta;
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(output_text, "final only");
}

TEST(OpenAiResponsesClientTest, MapsHttpErrorResponsesToAbslStatus) {
    const std::string body = "{\"error\":{\"message\":\"rate limited\"}}";
    const std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(status.message(), "rate limited");
}

TEST(OpenAiResponsesClientTest, RejectsApiKeyContainingNewlineBeforeLaunchingCurl) {
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key\noutput = /tmp/pwned",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = 1,
        .target = "/v1/responses",
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses api_key must not contain carriage return, newline, or NUL");
}

TEST(OpenAiResponsesClientTest, RejectsHeaderValuesContainingNewlineBeforeLaunchingCurl) {
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = 1,
        .target = "/v1/responses",
        .organization = "org_123\r\nX-Injected: yes",
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses organization must not contain carriage return, newline, or NUL");
}

TEST(OpenAiResponsesClientTest, RejectsUserAgentContainingNulBeforeLaunchingCurl) {
    std::string user_agent = "agent";
    user_agent.push_back('\0');
    user_agent += "suffix";
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = 1,
        .target = "/v1/responses",
        .user_agent = user_agent,
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses user_agent must not contain carriage return, newline, or NUL");
}

TEST(OpenAiResponsesClientTest, MapsAdditionalHttpErrorStatuses) {
    struct Case {
        std::string status_line;
        absl::StatusCode expected_code;
        std::string expected_message;
    };

    const std::array<Case, 4> cases = {
        Case{ "HTTP/1.1 401 Unauthorized", absl::StatusCode::kUnauthenticated, "bad key" },
        Case{ "HTTP/1.1 403 Forbidden", absl::StatusCode::kPermissionDenied, "forbidden" },
        Case{ "HTTP/1.1 422 Unprocessable Entity", absl::StatusCode::kInvalidArgument,
              "invalid prompt" },
        Case{ "HTTP/1.1 500 Internal Server Error", absl::StatusCode::kUnavailable,
              "server exploded" },
    };

    for (const Case& test_case : cases) {
        const std::string body =
            std::string("{\"error\":{\"message\":\"") + test_case.expected_message + "\"}}";
        const std::string response =
            test_case.status_line +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        OneShotHttpServer server(response);
        auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
            .enabled = true,
            .api_key = "test_key",
            .scheme = "http",
            .host = "127.0.0.1",
            .port = server.port(),
            .target = "/v1/responses",
        });

        const absl::Status status = client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.2",
                .system_prompt = "",
                .user_text = "hello",
            },
            [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

        ASSERT_FALSE(status.ok());
        EXPECT_EQ(status.code(), test_case.expected_code);
        EXPECT_EQ(status.message(), test_case.expected_message);
    }
}

TEST(OpenAiResponsesClientTest, MapsResponseFailedEventToUnavailableStatus) {
    const std::string body =
        "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"message\":\"provider "
        "failed\"}}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(status.message(), "provider failed");
}

TEST(OpenAiResponsesClientTest, RejectsMalformedSseJson) {
    const std::string body = "data: {not json}\r\n\r\n"
                             "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
    EXPECT_EQ(status.message(), "openai responses stream contained invalid JSON");
}

TEST(OpenAiResponsesClientTest, PropagatesCallbackFailure) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent& event) {
            return std::visit(
                [](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        return absl::CancelledError("stop streaming");
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kCancelled);
    EXPECT_EQ(status.message(), "stop streaming");
}

TEST(OpenAiResponsesClientTest, RejectsCompletedEventWithoutRecoverableText) {
    const std::string body =
        "data: "
        "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"output\":[]}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    std::string output_text;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.2",
            .system_prompt = "",
            .user_text = "hello",
        },
        [&](const OpenAiResponsesEvent& event) -> absl::Status {
            return std::visit(
                [&](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        output_text += concrete_event.text_delta;
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
    EXPECT_EQ(status.message(), "openai responses completed without any recoverable output text");
    EXPECT_TRUE(output_text.empty());
}

} // namespace
} // namespace isla::server::ai_gateway
