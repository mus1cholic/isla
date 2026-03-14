#include "isla/server/openai_responses_client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#endif
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include "openai_responses_inprocess_transport_test_hooks.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
#if !defined(_WIN32)
namespace ssl = asio::ssl;
#endif

constexpr auto kEarlyAbortTimeout = 2s;

void ReportTestServerThreadException(std::string_view server_name) {
    try {
        throw;
    } catch (const std::exception& error) {
        ADD_FAILURE() << server_name << " worker thread threw exception: " << error.what();
    } catch (...) {
        ADD_FAILURE() << server_name << " worker thread threw a non-std exception";
    }
}

bool IsExpectedTlsPeerAbort(const std::exception& error) {
    const std::string_view message = error.what();
    return message.find("alert unknown ca") != std::string_view::npos ||
           message.find("certificate unknown") != std::string_view::npos ||
           message.find("stream truncated") != std::string_view::npos;
}

void ReportHttpsTestServerThreadException() {
    try {
        throw;
    } catch (const std::exception& error) {
        if (IsExpectedTlsPeerAbort(error)) {
            return;
        }
        ADD_FAILURE() << "OneShotHttpsServer worker thread threw exception: " << error.what();
    } catch (...) {
        ADD_FAILURE() << "OneShotHttpsServer worker thread threw a non-std exception";
    }
}

class FixedStatusHostResolver final : public OpenAiResponsesHostResolver {
  public:
    explicit FixedStatusHostResolver(absl::Status status) : status_(std::move(status)) {}

    [[nodiscard]] absl::StatusOr<tcp::resolver::results_type>
    Resolve(asio::io_context* /*io_context*/, const OpenAiResponsesClientConfig& /*config*/,
            std::chrono::steady_clock::time_point /*deadline*/) const override {
        return status_;
    }

  private:
    absl::Status status_;
};

class StaticEndpointHostResolver final : public OpenAiResponsesHostResolver {
  public:
    explicit StaticEndpointHostResolver(tcp::endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    [[nodiscard]] absl::StatusOr<tcp::resolver::results_type>
    Resolve(asio::io_context* /*io_context*/, const OpenAiResponsesClientConfig& config,
            std::chrono::steady_clock::time_point /*deadline*/) const override {
        return tcp::resolver::results_type::create(endpoint_, config.host,
                                                   std::to_string(endpoint_.port()));
    }

  private:
    tcp::endpoint endpoint_;
};

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

class PausingHttpServer {
  public:
    explicit PausingHttpServer(std::vector<std::string> response_chunks,
                               std::shared_future<void> continue_future = {})
        : response_chunks_(std::move(response_chunks)),
          continue_future_(std::move(continue_future)),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~PausingHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForFirstChunkSent() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (first_chunk_sent_.load()) {
                return true;
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

            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");

            for (std::size_t i = 0; i < response_chunks_.size(); ++i) {
                boost::system::error_code write_error;
                asio::write(socket,
                            asio::buffer(response_chunks_[i].data(), response_chunks_[i].size()),
                            write_error);
                if (i == 0U) {
                    first_chunk_sent_.store(true);
                }
                if (write_error) {
                    break;
                }
                if (i == 0U && continue_future_.valid()) {
                    while (!stopped_.load()) {
                        if (continue_future_.wait_for(10ms) == std::future_status::ready) {
                            break;
                        }
                    }
                }
            }

            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
            ReportTestServerThreadException("PausingHttpServer");
        }
    }

    std::vector<std::string> response_chunks_;
    std::shared_future<void> continue_future_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::atomic<bool> first_chunk_sent_{ false };
    std::uint16_t port_ = 0;
};

class DelayedHeaderHttpServer {
  public:
    explicit DelayedHeaderHttpServer(std::string response, std::chrono::milliseconds response_delay)
        : response_(std::move(response)), response_delay_(response_delay),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~DelayedHeaderHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForRequest() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (request_received_.load()) {
                return true;
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

            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            request_received_.store(true);

            const auto deadline = std::chrono::steady_clock::now() + response_delay_;
            while (!stopped_.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(10ms);
            }

            boost::system::error_code write_error;
            asio::write(socket, asio::buffer(response_.data(), response_.size()), write_error);
            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
            ReportTestServerThreadException("DelayedHeaderHttpServer");
        }
    }

    std::string response_;
    std::chrono::milliseconds response_delay_;
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{ false };
    std::atomic<bool> request_received_{ false };
    std::uint16_t port_ = 0;
};

#if !defined(_WIN32)
constexpr char kTestTlsServerKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIG/wIBADANBgkqhkiG9w0BAQEFAASCBukwggblAgEAAoIBgQCylKlLaKU+hOvJ
DfriTRLd+IthG5hv28I3A/CGjLICT0rDDtgaXd0uqloJAnjsgn5gMAcStpDW8Rm+
t6LsrBL+5fBgkyU1r94Rvx0HHoyaZwBBouitVHw28hP3W+smddkqB1UxpGnTeL2B
gj3dVo/WTtRfO+0h0PKw1l98YE1pMTdqIwcOOE/ER0g4hvA/wrxuLhMvlVLMy/lL
58uctqaDUqryNyeerKbVkq4fJyCG5D2TwXVJ3i2DDh0xSt2Y10poZV4M4k8Su9Z5
8zN2PSvYMT50aqF277v8BaOeYUApBE4kZGIJpo13ATGdEwpUFZ0Fri4zLYUZ1hWb
OC35sKo7OxWQ/+tefNUdgWHob6Vmy777jiYcLwxc3sS9rF3AJe0rMW83kCkR6hmy
A3250E137N/1QumHuT/Nj9rnI/lwt9jfaYkZjoAgT/C97m/mM83cYpGTdoGV1xNo
7G90MhP0di5FnVsrIaSnvkbGT9UgUWx0oVMjocifdG2qIhMI9psCAwEAAQKCAYBT
sHmaPmNaZj59jZCqp0YVQlpHWwBYQ5vD3pPE6oCttm0p9nXt/VkfenQRTthOtmT1
POzDp00/feP7zeGLmqSYUjgRekPw4gdnN7Ip2PY5kdW77NWwDSzdLxuOS8Rq1MW9
/Yu+ZPe3RBlDbT8C0IM+Atlh/BqIQ3zIxN4g0pzUlF0M33d6AYfYSzOcUhibOO7H
j84r+YXBNkIRgYKZYbutRXuZYaGuqejRpBj3voVu0d3Ntdb6lCWuClpB9HzfGN0c
RTv8g6UYO4sK3qyFn90ibIR/1GB9watvtoWVZqggiWeBzSWVWRsGEf9O+Cx4oJw1
IphglhmhbgNksbj7bD24on/icldSOiVkoUemUOFmHWhCm4PnB1GmbD8YMfEdSbks
qDr1Ps1zg4mGOinVD/4cY7vuPFO/HCH07wfeaUGzRt4g0/yLr+XjVofOA3oowyxv
JAzr+niHA3lg5ecj4r7M68efwzN1OCyjMrVJw2RAzwvGxE+rm5NiT08SWlKQZnkC
gcEA4wvyLpIur/UB84nV3XVJ89UMNBLm++aTFzld047BLJtMaOhvNqx6Cl5c8VuW
l261KHjiVzpfNM3/A2LBQJcYkhX7avkqEXlj57cl+dCWAVwUzKmLJTPjfaTTZnYJ
xeN3dMYjJz2z2WtgvfvDoJLukVwIMmhTY8wtqqYyQBJ/l06pBsfw5TNvmVIOQHds
8ASOiFt+WRLk2bl9xrGGayqt3VV93KVRzF27cpjOgEcG74F3c0ZW9snERN7vIYwB
JfrlAoHBAMlahPwMP2TYylG8OzHe7EiehTekSO26LGh0Cq3wTGXYsK/q8hQCzL14
kWW638vpwXL6L9ntvrd7hjzWRO3vX/VxnYEA6f0bpqHq1tZi6lzix5CTUN5McpDg
QnjenSJNrNjS1zEF8WeY9iLEuDI/M/iUW4y9R6s3WpgQhPDXpSvd2g3gMGRUYhxQ
Xna8auiJeYFq0oNaOxvJj+VeOfJ3ZMJttd+Y7gTOYZcbg3SdRb/kdxYki0RMD2hF
4ZvjJ6CTfwKBwQDiMqiZFTJGQwYqp4vWEmAW+I4r4xkUpWatoI2Fk5eI5T9+1PLX
uYXsho56NxEU1UrOg4Cb/p+TcBc8PErkGqR0BkpxDMOInTOXSrQe6lxIBoECVXc3
HTbrmiay0a5y5GfCgxPKqIJhfcToAceoVjovv0y7S4yoxGZKuUEe7E8JY2iqRNAO
yOvKCCICv/hcN235E44RF+2/rDlOltagNej5tY6rIFkaDdgOF4bD7f9O5eEni1Bg
litfoesDtQP/3rECgcEAkQfvQ7D6tIPmbqsbJBfCr6fmoqZllT4FIJN84b50+OL0
mTGsfjdqC4tdhx3sdu7/VPbaIqm5NmX10bowWgWSY7MbVME4yQPyqSwC5NbIonEC
d6N0mzoLR0kQ+Ai4u+2g82gicgAq2oj1uSNi3WZi48jQjHYFulCbo246o1NgeFFK
77WshYe2R1ioQfQDOU1URKCR0uTaMHClgfu112yiGd12JAD+aF3TM0kxDXz+sXI5
SKy311DFxECZeXRLpcC3AoHBAJkNMJWTyPYbeVu+CTQkec8Uun233EkXa2kUNZc/
5DuXDaK+A3DMgYRufTKSPpDHGaCZ1SYPInX1Uoe2dgVjWssRL2uitR4ENabDoAOA
ICVYXYYNagqQu5wwirF0QeaMXo1fjhuuHQh8GsMdXZvYEaAITZ9/NG5x/oY08+8H
kr78SMBOPy3XQn964uKG+e3JwpOG14GKABdAlrHKFXNWchu/6dgcYXB87mrC/GhO
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

class OneShotHttpsServer {
  public:
    explicit OneShotHttpsServer(std::string response)
        : response_(std::move(response)), ssl_context_(ssl::context::tls_server),
          acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        boost::system::error_code error;
        ssl_context_.use_certificate_chain(asio::buffer(kTestTlsServerCertPem), error);
        if (error) {
            throw std::runtime_error("failed to configure test TLS certificate chain: " +
                                     error.message());
        }
        ssl_context_.use_private_key(asio::buffer(kTestTlsServerKeyPem), ssl::context::pem, error);
        if (error) {
            throw std::runtime_error("failed to configure test TLS private key: " +
                                     error.message());
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
                asio::read(ssl_stream, asio::buffer(tail.data(), tail.size()));
                request += tail;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_text_ = request;
            }

            asio::write(ssl_stream, asio::buffer(response_.data(), response_.size()));
            boost::system::error_code error;
            ssl_stream.shutdown(error);
            ssl_stream.next_layer().shutdown(tcp::socket::shutdown_both, error);
            ssl_stream.next_layer().close(error);
        } catch (...) {
            ReportHttpsTestServerThreadException();
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
            .model = "gpt-5.4",
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
    EXPECT_NE(server.request_text().find("\"model\":\"gpt-5.4\""), std::string::npos);
    EXPECT_NE(server.request_text().find("\"instructions\":\"system prompt\""), std::string::npos);
}

TEST(OpenAiResponsesClientTest, StreamsSseWhenEventPayloadIsSplitAcrossReadChunks) {
    const std::string first_chunk = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/event-stream\r\n"
                                    "Connection: close\r\n\r\n"
                                    "data: {\"type\":\"response.output_text.del";
    const std::string second_chunk = "ta\",\"delta\":\"hello ";
    const std::string third_chunk =
        "world\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    PausingHttpServer server({ first_chunk, second_chunk, third_chunk });
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
            .model = "gpt-5.4",
            .system_prompt = "",
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
    EXPECT_EQ(output_text, "hello world");
    EXPECT_EQ(completed_count, 1);
}

TEST(OpenAiResponsesClientTest, UsesRealDnsResolutionForLocalhostHttpTransport) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"dns ok\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_dns\"}}\r\n\r\n"
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
        .host = "localhost",
        .port = server.port(),
        .target = "/v1/responses",
    });

    std::string output_text;
    int completed_count = 0;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
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
    EXPECT_EQ(output_text, "dns ok");
    EXPECT_EQ(completed_count, 1);
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
            .model = "gpt-5.4",
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
            .model = "gpt-5.4",
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

#if !defined(_WIN32)
TEST(OpenAiResponsesClientTest, UsesRealDnsResolutionForLocalhostHttpsTransportWithInjectedCa) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"secure \"}\r\n\r\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"world\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_tls\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpsServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "https",
        .host = "localhost",
        .port = server.port(),
        .target = "/v1/responses",
        .trusted_ca_cert_pem = std::string(kTestTlsServerCertPem),
    });

    std::string output_text;
    int completed_count = 0;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
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
    EXPECT_EQ(output_text, "secure world");
    EXPECT_EQ(completed_count, 1);
    EXPECT_NE(server.request_text().find("POST /v1/responses HTTP/1.1"), std::string::npos);
    EXPECT_NE(server.request_text().find("Authorization: Bearer test_key"), std::string::npos);
}

TEST(OpenAiResponsesClientTest, RejectsHttpsServerCertificateWithoutInjectedCa) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ignored\"}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpsServer server(response);
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "https",
        .host = "localhost",
        .port = server.port(),
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_NE(std::string(status.message()).find("TLS handshake"), std::string::npos);
}
#endif

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
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(status.message(), "rate limited");
}

TEST(OpenAiResponsesClientTest, RejectsOversizedHttpResponseHeaders) {
    const std::string oversized_header_value(20U * 1024U, 'a');
    const std::string response =
        "HTTP/1.1 200 OK\r\nX-Fill: " + oversized_header_value + "\r\nConnection: close\r\n\r\n";
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
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(status.message(),
              "openai responses transport response header exceeds maximum length");
}

TEST(OpenAiResponsesClientTest, RejectsApiKeyContainingNewlineBeforeStartingTransport) {
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

TEST(OpenAiResponsesClientTest, RejectsHeaderValuesContainingNewlineBeforeStartingTransport) {
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

TEST(OpenAiResponsesClientTest, RejectsUserAgentContainingNulBeforeStartingTransport) {
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
                .model = "gpt-5.4",
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
            .model = "gpt-5.4",
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
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
    EXPECT_EQ(status.message(), "openai responses stream contained invalid JSON");
}

TEST(OpenAiResponsesClientTest, RejectsSseEventsWithWrongTypedStringFields) {
    const std::string body = "data: {\"type\":123}\r\n\r\n"
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
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
    EXPECT_EQ(status.message(), "openai responses event field 'type' must be a string");
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
            .model = "gpt-5.4",
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
            .model = "gpt-5.4",
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

TEST(OpenAiResponsesClientTest, AbortsTransportWhenCallbackReturnsNonOk) {
    auto continue_promise = std::make_shared<std::promise<void>>();
    const std::string first_chunk =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\r\n\r\n";
    const std::string second_chunk =
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    PausingHttpServer server({ first_chunk, second_chunk }, continue_promise->get_future().share());
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    auto response_future = std::async(std::launch::async, [&] {
        return client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.4",
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
    });

    ASSERT_TRUE(server.WaitForFirstChunkSent());
    // This is intentionally generous for loaded CI runners. The assertion is
    // about early termination before the remaining bytes are released, not
    // about measuring transport latency.
    ASSERT_EQ(response_future.wait_for(kEarlyAbortTimeout), std::future_status::ready);

    continue_promise->set_value();
    const absl::Status status = response_future.get();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kCancelled);
    EXPECT_EQ(status.message(), "stop streaming");
}

TEST(OpenAiResponsesClientTest, AbortsTransportAfterCompletedEventWithoutWaitingForDone) {
    auto continue_promise = std::make_shared<std::promise<void>>();
    const std::string first_chunk =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\r\n\r\n";
    const std::string second_chunk = "data: [DONE]\r\n\r\n";
    PausingHttpServer server({ first_chunk, second_chunk }, continue_promise->get_future().share());
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
    auto response_future = std::async(std::launch::async, [&] {
        return client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.4",
                .system_prompt = "",
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
    });

    ASSERT_TRUE(server.WaitForFirstChunkSent());
    ASSERT_EQ(response_future.wait_for(kEarlyAbortTimeout), std::future_status::ready);

    continue_promise->set_value();
    const absl::Status status = response_future.get();
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(output_text, "hello");
    EXPECT_EQ(completed_count, 1);
}

TEST(OpenAiResponsesClientTest, AbortsTransportWhenBodyBudgetIsExceeded) {
    auto continue_promise = std::make_shared<std::promise<void>>();
    const std::string oversized_body(300U * 1024U, 'x');
    const std::string first_chunk = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/event-stream\r\n"
                                    "Connection: close\r\n\r\n" +
                                    oversized_body;
    PausingHttpServer server({ first_chunk }, continue_promise->get_future().share());
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
    });

    auto response_future = std::async(std::launch::async, [&] {
        return client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.4",
                .system_prompt = "",
                .user_text = "hello",
            },
            [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });
    });

    ASSERT_TRUE(server.WaitForFirstChunkSent());
    ASSERT_EQ(response_future.wait_for(kEarlyAbortTimeout), std::future_status::ready);

    continue_promise->set_value();
    const absl::Status status = response_future.get();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(status.message(), "openai responses transport body exceeds maximum length");
}

TEST(OpenAiResponsesClientTest, DeterministicallyUsesInjectedResolverSuccessEndpoints) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"resolver ok\"}\r\n\r\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_resolver\"}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    OneShotHttpServer server(response);
    auto resolver = std::make_shared<StaticEndpointHostResolver>(
        tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
    ScopedOpenAiResponsesHostResolverOverrideForTest scoped_resolver(resolver);

    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "resolver.override.test",
        .port = 80,
        .target = "/v1/responses",
    });

    std::string output_text;
    int completed_count = 0;
    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
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
    EXPECT_EQ(output_text, "resolver ok");
    EXPECT_EQ(completed_count, 1);
    EXPECT_NE(server.request_text().find("Host: resolver.override.test"), std::string::npos);
}

TEST(OpenAiResponsesClientTest, TimesOutWhenResponseHeadersDoNotArriveBeforeDeadline) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"late\"}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n"
                                 "Connection: close\r\n\r\n" +
                                 body;
    DelayedHeaderHttpServer server(response, std::chrono::milliseconds(500));
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = server.port(),
        .target = "/v1/responses",
        .request_timeout = std::chrono::milliseconds(100),
    });

    auto response_future = std::async(std::launch::async, [&] {
        return client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.4",
                .system_prompt = "",
                .user_text = "hello",
            },
            [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });
    });

    ASSERT_TRUE(server.WaitForRequest());
    ASSERT_EQ(response_future.wait_for(kEarlyAbortTimeout), std::future_status::ready);

    const absl::Status status = response_future.get();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kDeadlineExceeded);
    EXPECT_NE(
        std::string(status.message()).find("failed to read openai responses HTTP response header"),
        std::string::npos);
    EXPECT_NE(std::string(status.message()).find("timeout"), std::string::npos);
}

TEST(OpenAiResponsesClientTest, DeterministicallySurfacesDnsTimeoutFromInjectedResolver) {
    auto resolver = std::make_shared<FixedStatusHostResolver>(
        absl::DeadlineExceededError("openai responses request timed out during DNS resolution"));
    ScopedOpenAiResponsesHostResolverOverrideForTest scoped_resolver(resolver);

    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "example.com",
        .port = 80,
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kDeadlineExceeded);
    EXPECT_EQ(status.message(), "openai responses request timed out during DNS resolution");
}

TEST(OpenAiResponsesClientTest, DeterministicallySurfacesDnsResolutionFailureFromInjectedResolver) {
    auto resolver = std::make_shared<FixedStatusHostResolver>(
        absl::UnavailableError("failed to resolve openai responses host: host not found"));
    ScopedOpenAiResponsesHostResolverOverrideForTest scoped_resolver(resolver);

    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "example.com",
        .port = 80,
        .target = "/v1/responses",
    });

    const absl::Status status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(status.message(), "failed to resolve openai responses host: host not found");
}

TEST(OpenAiResponsesClientTest, RestoresPreviousResolverOverrideWhenNestedScopesExit) {
    auto outer_resolver = std::make_shared<FixedStatusHostResolver>(
        absl::UnavailableError("failed to resolve openai responses host: outer"));
    auto inner_resolver = std::make_shared<FixedStatusHostResolver>(
        absl::DeadlineExceededError("openai responses request timed out during DNS resolution"));

    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "example.com",
        .port = 80,
        .target = "/v1/responses",
    });

    ScopedOpenAiResponsesHostResolverOverrideForTest outer_scope(outer_resolver);
    {
        ScopedOpenAiResponsesHostResolverOverrideForTest inner_scope(inner_resolver);
        const absl::Status inner_status = client->StreamResponse(
            OpenAiResponsesRequest{
                .model = "gpt-5.4",
                .system_prompt = "",
                .user_text = "hello",
            },
            [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

        ASSERT_FALSE(inner_status.ok());
        EXPECT_EQ(inner_status.code(), absl::StatusCode::kDeadlineExceeded);
        EXPECT_EQ(inner_status.message(),
                  "openai responses request timed out during DNS resolution");
    }

    const absl::Status restored_status = client->StreamResponse(
        OpenAiResponsesRequest{
            .model = "gpt-5.4",
            .system_prompt = "",
            .user_text = "hello",
        },
        [](const OpenAiResponsesEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(restored_status.ok());
    EXPECT_EQ(restored_status.code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(restored_status.message(), "failed to resolve openai responses host: outer");
}

TEST(OpenAiResponsesClientTest, RejectsTargetContainingNewlineBeforeStartingTransport) {
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = 1,
        .target = "/v1/responses\r\nX-Injected: yes",
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses target must not contain carriage return, newline, or NUL");
}

TEST(OpenAiResponsesClientTest, RejectsHostContainingSpaceBeforeStartingTransport) {
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "local host",
        .port = 1,
        .target = "/v1/responses",
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses host must not contain ASCII whitespace or control characters");
}

TEST(OpenAiResponsesClientTest, RejectsTargetContainingSpaceBeforeStartingTransport) {
    auto client = CreateOpenAiResponsesClient(OpenAiResponsesClientConfig{
        .enabled = true,
        .api_key = "test_key",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = 1,
        .target = "/v1/responses bad",
    });

    const absl::Status status = client->Validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(),
              "openai responses target must not contain ASCII whitespace or control characters");
}

} // namespace
} // namespace isla::server::ai_gateway
