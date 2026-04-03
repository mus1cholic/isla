#ifndef ISLA_SERVER_SRC_ONE_SHOT_HTTP_SERVER_TEST_UTILS_HPP_
#define ISLA_SERVER_SRC_ONE_SHOT_HTTP_SERVER_TEST_UTILS_HPP_

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

namespace isla::server::test {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

inline void ReportOneShotHttpServerThreadException() {
    try {
        throw;
    } catch (const std::exception& error) {
        std::string message = error.what();
        std::string lowered = message;
        for (char& ch : lowered) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        // NOTICE: Some negative-path client tests intentionally close the socket before the test
        // server finishes writing its response. Those peer-abort errors are expected and should
        // not fail the test helper itself.
        if (lowered.find("10054") != std::string::npos ||
            lowered.find("broken pipe") != std::string::npos ||
            lowered.find("connection reset by peer") != std::string::npos) {
            return;
        }
        ADD_FAILURE() << "OneShotHttpServer worker thread threw exception: " << message;
    } catch (...) {
        ADD_FAILURE() << "OneShotHttpServer worker thread threw a non-std exception";
    }
}

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

    static std::string ReadCompleteRequest(tcp::socket* socket) {
        asio::streambuf buffer;
        asio::read_until(*socket, buffer, "\r\n\r\n");

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
                const std::size_t value_begin = content_length_pos + content_length_prefix.size();
                const std::size_t value_end = headers.find("\r\n", value_begin);
                content_length = static_cast<std::size_t>(
                    std::stoul(headers.substr(value_begin, value_end - value_begin)));
            }
        }

        const std::size_t body_already_buffered =
            header_end == std::string::npos ? 0U : request.size() - (header_end + 4U);
        if (body_already_buffered < content_length) {
            std::string tail(content_length - body_already_buffered, '\0');
            asio::read(*socket, asio::buffer(tail.data(), tail.size()));
            request += tail;
        }
        return request;
    }

    void Run() {
        try {
            tcp::socket socket(io_context_);
            acceptor_.accept(socket);

            const std::string request = ReadCompleteRequest(&socket);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_text_ = request;
            }

            WriteResponse(&socket);

            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
            ReportOneShotHttpServerThreadException();
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

} // namespace isla::server::test

#endif // ISLA_SERVER_SRC_ONE_SHOT_HTTP_SERVER_TEST_UTILS_HPP_
