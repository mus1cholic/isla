#include "isla/server/ollama_llm_client.hpp"

#include <chrono>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using nlohmann::json;
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

    [[nodiscard]] bool WaitForRequest() const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request_text_.has_value()) {
                return true;
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

            const std::size_t header_end = request.find("\r\n\r\n");
            std::size_t content_length = 0U;
            if (header_end != std::string::npos) {
                const std::string_view headers(request.data(), header_end);
                constexpr std::string_view kContentLengthHeader = "Content-Length: ";
                const std::size_t content_length_pos = headers.find(kContentLengthHeader);
                if (content_length_pos != std::string::npos) {
                    const std::size_t value_begin = content_length_pos + kContentLengthHeader.size();
                    const std::size_t value_end = headers.find("\r\n", value_begin);
                    content_length = static_cast<std::size_t>(std::stoull(
                        std::string(headers.substr(value_begin, value_end - value_begin))));
                }
            }

            const std::size_t body_already_buffered =
                header_end == std::string::npos ? 0U : request.size() - (header_end + 4U);
            if (body_already_buffered < content_length) {
                std::string tail(content_length - body_already_buffered, '\0');
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
    mutable std::optional<std::string> request_text_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

std::string MakeJsonResponse(unsigned int status_code, std::string_view status_text,
                             std::string body) {
    return "HTTP/1.1 " + std::to_string(status_code) + " " + std::string(status_text) + "\r\n" +
           "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

std::string ExtractRequestBody(std::string_view request_text) {
    const std::size_t body_begin = request_text.find("\r\n\r\n");
    if (body_begin == std::string_view::npos) {
        return "";
    }
    return std::string(request_text.substr(body_begin + 4U));
}

TEST(OllamaLlmClientTest, FactoryRejectsDisabledConfig) {
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = false,
        });

    ASSERT_FALSE(client.ok());
    EXPECT_EQ(client.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(OllamaLlmClientTest, StreamsChatResponseAndOmitsAuthorizationWhenUnset) {
    const std::string response_body = R"json({
        "model":"qwen3:4b",
        "created_at":"resp_ollama_1",
        "message":{"role":"assistant","content":"hello from qwen"},
        "done":true
    })json";
    OneShotHttpServer server(MakeJsonResponse(200, "OK", response_body));
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    std::vector<std::string> deltas;
    std::string response_id;
    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "qwen3:4b",
            .system_prompt = "system prompt",
            .user_text = "hello",
            .reasoning_effort = LlmReasoningEffort::kNone,
        },
        [&deltas, &response_id](const LlmEvent& event) -> absl::Status {
            return std::visit(
                [&deltas, &response_id](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, LlmTextDeltaEvent>) {
                        deltas.push_back(concrete_event.text_delta);
                    }
                    if constexpr (std::is_same_v<Event, LlmCompletedEvent>) {
                        response_id = concrete_event.response_id;
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequest());
    const std::string request_text = server.request_text();
    EXPECT_NE(request_text.find("POST /api/chat HTTP/1.1"), std::string::npos);
    EXPECT_EQ(request_text.find("Authorization: Bearer "), std::string::npos);

    const json request_body = json::parse(ExtractRequestBody(request_text));
    EXPECT_EQ(request_body.at("model"), "qwen3:4b");
    EXPECT_EQ(request_body.at("stream"), false);
    EXPECT_EQ(request_body.at("think"), false);
    ASSERT_EQ(request_body.at("messages").size(), 2U);
    EXPECT_EQ(request_body.at("messages")[0].at("role"), "system");
    EXPECT_EQ(request_body.at("messages")[0].at("content"), "system prompt");
    EXPECT_EQ(request_body.at("messages")[1].at("role"), "user");
    EXPECT_EQ(request_body.at("messages")[1].at("content"), "hello");

    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_EQ(deltas[0], "hello from qwen");
    EXPECT_EQ(response_id, "resp_ollama_1");
}

TEST(OllamaLlmClientTest, IncludesAuthorizationAndEnablesThinkingForReasoningRequests) {
    const std::string response_body = R"json({
        "message":{"role":"assistant","content":"ready"},
        "done":true
    })json";
    OneShotHttpServer server(MakeJsonResponse(200, "OK", response_body));
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
            .api_key = std::string("secret"),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "qwen3:4b",
            .user_text = "hello",
            .reasoning_effort = LlmReasoningEffort::kMedium,
        },
        [](const LlmEvent&) { return absl::OkStatus(); });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequest());
    const std::string request_text = server.request_text();
    EXPECT_NE(request_text.find("Authorization: Bearer secret"), std::string::npos);

    const json request_body = json::parse(ExtractRequestBody(request_text));
    EXPECT_EQ(request_body.at("think"), true);
}

TEST(OllamaLlmClientTest, MapsHttpErrorsToAbslStatuses) {
    const std::string response_body = R"json({"error":"model not found"})json";
    OneShotHttpServer server(MakeJsonResponse(404, "Not Found", response_body));
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "missing-model",
            .user_text = "hello",
        },
        [](const LlmEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "model not found");
}

} // namespace
} // namespace isla::server
