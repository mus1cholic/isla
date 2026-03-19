#include "isla/server/gemini_api_embedding_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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

void ReportTestServerThreadException(std::string_view server_name) {
    try {
        throw;
    } catch (const std::exception& error) {
        ADD_FAILURE() << server_name << " worker thread threw exception: " << error.what();
    } catch (...) {
        ADD_FAILURE() << server_name << " worker thread threw a non-std exception";
    }
}

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
            ReportTestServerThreadException("OneShotHttpServer");
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

GeminiApiEmbeddingClientConfig MakeConfig(std::uint16_t port) {
    return GeminiApiEmbeddingClientConfig{
        .enabled = true,
        .api_key = "api_key_test",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = port,
        .request_timeout = 2s,
    };
}

TEST(GeminiApiEmbeddingClientTest, ValidateRejectsMissingApiKey) {
    const absl::Status status = ValidateGeminiApiEmbeddingClientConfig(
        GeminiApiEmbeddingClientConfig{
            .enabled = true,
            .api_key = "",
        });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(GeminiApiEmbeddingClientTest, EmbedPostsEmbedContentRequestAndParsesEmbedding) {
    const std::string response_body =
        R"({"embedding":{"values":[0.25,-0.5,1.75]}})";
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();
    ASSERT_NE(*client, nullptr);

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
    });

    ASSERT_TRUE(embedding.ok()) << embedding.status();
    EXPECT_EQ(*embedding, (memory::Embedding{ 0.25, -0.5, 1.75 }));
    ASSERT_TRUE(server.WaitForRequest());
    const std::string request = server.request_text();
    EXPECT_NE(
        request.find("POST /v1beta/models/gemini-embedding-2-preview:embedContent HTTP/1.1"),
        std::string::npos);
    EXPECT_NE(request.find("x-goog-api-key: api_key_test"), std::string::npos);
    const std::size_t body_pos = request.find("\r\n\r\n");
    ASSERT_NE(body_pos, std::string::npos);
    const json body = json::parse(request.substr(body_pos + 4U));
    ASSERT_TRUE(body.contains("content"));
    ASSERT_TRUE(body["content"].contains("parts"));
    ASSERT_EQ(body["content"]["parts"].size(), 1U);
    EXPECT_EQ(body["content"]["parts"][0]["text"], "debugged the export crash");
}

TEST(GeminiApiEmbeddingClientTest, EmbedMapsHttpFailures) {
    const std::string response_body = R"({"error":{"message":"quota exceeded"}})";
    OneShotHttpServer server(
        "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_NE(std::string(embedding.status().message()).find("quota exceeded"), std::string::npos);
}

TEST(GeminiApiEmbeddingClientTest, EmbedRejectsMalformedResponses) {
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: 2\r\n\r\n{}");
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server
