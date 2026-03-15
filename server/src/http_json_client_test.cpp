#include "http_json_client.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

namespace isla::server {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

void ReportTestServerThreadException(std::string_view server_name) {
    try {
        throw;
    } catch (const std::exception& error) {
        std::string message = error.what();
        std::string lowered = message;
        for (char& ch : lowered) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lowered.find("10054") != std::string::npos ||
            lowered.find("broken pipe") != std::string::npos ||
            lowered.find("connection reset by peer") != std::string::npos) {
            return;
        }
        ADD_FAILURE() << server_name << " worker thread threw exception: " << message;
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

class OneShotHttpServer {
  public:
    explicit OneShotHttpServer(std::string response)
        : response_(std::move(response)), acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    virtual ~OneShotHttpServer() {
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

  protected:
    virtual void WriteResponse(tcp::socket* socket) {
        asio::write(*socket, asio::buffer(response_.data(), response_.size()));
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

            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_text_ = request;
            }
            WriteResponse(&socket);

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

class DelayedHeaderHttpServer final : public OneShotHttpServer {
  public:
    DelayedHeaderHttpServer(std::chrono::milliseconds delay, std::string response)
        : OneShotHttpServer(std::move(response)), delay_(delay) {}

  protected:
    void WriteResponse(tcp::socket* socket) override {
        std::this_thread::sleep_for(delay_);
        OneShotHttpServer::WriteResponse(socket);
    }

  private:
    std::chrono::milliseconds delay_;
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

TEST(HttpJsonClientTest, ReturnsNon2xxResponseBodyWithoutMapping) {
    const std::string body = "{\"message\":\"rate limited\"}";
    OneShotHttpServer server("HTTP/1.1 429 Too Many Requests\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 2s,
                               .user_agent = "isla-http-json-client-test",
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "/rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_TRUE(response.ok()) << response.status();
    EXPECT_EQ(response->status_code, 429U);
    EXPECT_EQ(response->body, body);
    EXPECT_TRUE(server.WaitForRequest());
}

TEST(HttpJsonClientTest, ParseHttpUrlSupportsBracketedIpv6WithExplicitPort) {
    const absl::StatusOr<ParsedHttpUrl> parsed = ParseHttpUrl("https://[::1]:8443", "test url");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->scheme, "https");
    EXPECT_EQ(parsed->host, "[::1]");
    EXPECT_EQ(parsed->port, 8443);
}

TEST(HttpJsonClientTest, ParseHttpUrlSupportsBracketedIpv6WithoutExplicitPort) {
    const absl::StatusOr<ParsedHttpUrl> parsed = ParseHttpUrl("http://[::1]", "test url");

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->scheme, "http");
    EXPECT_EQ(parsed->host, "[::1]");
    EXPECT_EQ(parsed->port, 80);
}

TEST(HttpJsonClientTest, RejectsTargetPathWithoutLeadingSlash) {
    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:8080", "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 2s,
                               .user_agent = "isla-http-json-client-test",
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(response.status().message(), "HTTP request target_path must start with '/'");
}

TEST(HttpJsonClientTest, RejectsOversizedResponseHeaders) {
    const std::string oversized_header(20 * 1024, 'a');
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nX-Oversized: " + oversized_header +
                             "\r\nContent-Length: 0\r\n\r\n");

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 2s,
                               .user_agent = "isla-http-json-client-test",
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "/rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_NE(response.status().message().find("header exceeds maximum length"), std::string::npos);
}

TEST(HttpJsonClientTest, RejectsOversizedResponseBody) {
    const std::string oversized_body((17 * 1024 * 1024), 'x');
    OneShotHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(oversized_body.size()) + "\r\n\r\n" + oversized_body);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 2s,
                               .user_agent = "isla-http-json-client-test",
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "/rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_NE(response.status().message().find("body exceeds maximum length"), std::string::npos);
}

TEST(HttpJsonClientTest, TimesOutWhenResponseHeadersDoNotArriveBeforeDeadline) {
    DelayedHeaderHttpServer server(200ms, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 100ms,
                               .user_agent = "isla-http-json-client-test",
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "/rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kDeadlineExceeded);
}

class MultiRequestHttpServer {
  public:
    MultiRequestHttpServer(std::string response, int max_requests)
        : response_(std::move(response)), max_requests_(max_requests),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~MultiRequestHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] int requests_served() const {
        return requests_served_.load();
    }

    [[nodiscard]] int connections_accepted() const {
        return connections_accepted_.load();
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
                boost::system::error_code accept_error;
                tcp::socket socket(io_context_);
                acceptor_.accept(socket, accept_error);
                if (accept_error) {
                    break;
                }
                connections_accepted_.fetch_add(1);
                HandleConnection(std::move(socket));
            }
        } catch (...) {
            ReportTestServerThreadException("MultiRequestHttpServer");
        }
    }

    void HandleConnection(tcp::socket socket) {
        try {
            for (int i = 0; i < max_requests_ && !stopped_.load(); ++i) {
                asio::streambuf buffer;
                boost::system::error_code read_error;
                asio::read_until(socket, buffer, "\r\n\r\n", read_error);
                if (read_error) {
                    break;
                }

                // Extract buffered data (may include body bytes past the header boundary).
                std::string buffered_data;
                {
                    std::istream stream(&buffer);
                    buffered_data.assign(std::istreambuf_iterator<char>(stream),
                                         std::istreambuf_iterator<char>());
                }
                const auto header_end = buffered_data.find("\r\n\r\n");
                const std::size_t body_bytes_buffered = buffered_data.size() - (header_end + 4U);

                // Drain any remaining body from the socket based on Content-Length.
                const auto cl_pos = buffered_data.find("Content-Length: ");
                if (cl_pos != std::string::npos) {
                    const std::size_t content_length = std::stoull(buffered_data.substr(
                        cl_pos + 16U, buffered_data.find("\r\n", cl_pos + 16U) - (cl_pos + 16U)));
                    if (content_length > body_bytes_buffered) {
                        std::vector<char> remaining(content_length - body_bytes_buffered);
                        asio::read(socket, asio::buffer(remaining), read_error);
                    }
                }

                asio::write(socket, asio::buffer(response_.data(), response_.size()));
                requests_served_.fetch_add(1);
            }
            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
            ReportTestServerThreadException("MultiRequestHttpServer::HandleConnection");
        }
    }

    std::string response_;
    int max_requests_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::atomic<int> requests_served_{ 0 };
    std::atomic<int> connections_accepted_{ 0 };
    std::uint16_t port_ = 0;
};

TEST(PersistentHttpClientTest, ReusesConnectionForMultipleRequests) {
    const std::string body = "{\"ok\":true}";
    MultiRequestHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Connection: keep-alive\r\nContent-Length: " +
                                      std::to_string(body.size()) + "\r\n\r\n" + body,
                                  10);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    PersistentHttpClient client(*parsed_url, HttpClientConfig{
                                                 .request_timeout = 2s,
                                                 .user_agent = "isla-persistent-test",
                                             });

    for (int i = 0; i < 5; ++i) {
        const absl::StatusOr<HttpResponse> response = client.Execute(HttpRequestSpec{
            .method = boost::beast::http::verb::get,
            .target_path = "/rest/v1/test",
        });
        ASSERT_TRUE(response.ok()) << "request " << i << " failed: " << response.status();
        EXPECT_EQ(response->status_code, 200U);
        EXPECT_EQ(response->body, body);
    }

    EXPECT_EQ(server.requests_served(), 5);
    EXPECT_EQ(server.connections_accepted(), 1);
}

TEST(PersistentHttpClientTest, ReconnectsAfterServerClosesConnection) {
    const std::string body = "{\"ok\":true}";
    // Server closes connection after each request (max_requests=1), forcing reconnect.
    MultiRequestHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Connection: close\r\nContent-Length: " +
                                      std::to_string(body.size()) + "\r\n\r\n" + body,
                                  1);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    PersistentHttpClient client(*parsed_url, HttpClientConfig{
                                                 .request_timeout = 2s,
                                                 .user_agent = "isla-persistent-test",
                                             });

    for (int i = 0; i < 3; ++i) {
        const absl::StatusOr<HttpResponse> response = client.Execute(HttpRequestSpec{
            .method = boost::beast::http::verb::get,
            .target_path = "/rest/v1/test",
        });
        ASSERT_TRUE(response.ok()) << "request " << i << " failed: " << response.status();
        EXPECT_EQ(response->status_code, 200U);
    }

    EXPECT_EQ(server.requests_served(), 3);
    EXPECT_EQ(server.connections_accepted(), 3);
}

TEST(PersistentHttpClientTest, HandlesPostRequestsWithBody) {
    const std::string response_body = "{\"id\":1}";
    MultiRequestHttpServer server("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
                                  "Connection: keep-alive\r\nContent-Length: " +
                                      std::to_string(response_body.size()) + "\r\n\r\n" +
                                      response_body,
                                  10);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("http://127.0.0.1:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    PersistentHttpClient client(*parsed_url, HttpClientConfig{
                                                 .request_timeout = 2s,
                                                 .user_agent = "isla-persistent-test",
                                             });

    for (int i = 0; i < 3; ++i) {
        const absl::StatusOr<HttpResponse> response = client.Execute(HttpRequestSpec{
            .method = boost::beast::http::verb::post,
            .target_path = "/rest/v1/items",
            .body = "{\"name\":\"test\"}",
        });
        ASSERT_TRUE(response.ok()) << "request " << i << " failed: " << response.status();
        EXPECT_EQ(response->status_code, 201U);
        EXPECT_EQ(response->body, response_body);
    }

    EXPECT_EQ(server.requests_served(), 3);
    EXPECT_EQ(server.connections_accepted(), 1);
}

#if !defined(_WIN32)
TEST(HttpJsonClientTest, PerformsHttpsRequestWithInjectedTrust) {
    const std::string body = "{\"ok\":true}";
    OneShotHttpsServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body);

    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl("https://localhost:" + std::to_string(server.port()), "test url");
    ASSERT_TRUE(parsed_url.ok()) << parsed_url.status();

    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(*parsed_url,
                           HttpClientConfig{
                               .request_timeout = 2s,
                               .user_agent = "isla-http-json-client-test",
                               .trusted_ca_cert_pem = std::string(kTestTlsServerCertPem),
                           },
                           HttpRequestSpec{
                               .method = boost::beast::http::verb::get,
                               .target_path = "/rest/v1/test",
                               .query_parameters = {},
                               .headers = {},
                               .body = std::nullopt,
                           });

    ASSERT_TRUE(response.ok()) << response.status();
    EXPECT_EQ(response->status_code, 200U);
    EXPECT_EQ(response->body, body);
    EXPECT_TRUE(server.WaitForRequest());
}
#endif

} // namespace
} // namespace isla::server
