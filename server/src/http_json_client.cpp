#include "http_json_client.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace isla::server {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
#if !defined(_WIN32)
namespace ssl = asio::ssl;
#endif

constexpr std::size_t kMaxResponseHeaderBytes = 16U * 1024U;
constexpr std::uint64_t kMaxResponseBodyBytes = 16U * 1024U * 1024U;

using TransportDeadline = std::chrono::steady_clock::time_point;

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

absl::Status UnavailableTransportError(std::string_view prefix,
                                       const boost::system::error_code& error) {
    if (error == beast::error::timeout || error == asio::error::timed_out) {
        return absl::DeadlineExceededError(std::string(prefix) + ": " + error.message());
    }
    return absl::UnavailableError(std::string(prefix) + ": " + error.message());
}

std::string PercentEncode(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 3U);
    constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4U) & 0x0FU]);
        encoded.push_back(kHex[ch & 0x0FU]);
    }
    return encoded;
}

TransportDeadline ComputeTransportDeadline(const HttpClientConfig& config) {
    return std::chrono::steady_clock::now() + config.request_timeout;
}

void SetTransportDeadline(beast::tcp_stream* stream, TransportDeadline deadline) {
    stream->expires_at(deadline);
}

#if !defined(_WIN32)
void SetTransportDeadline(beast::ssl_stream<beast::tcp_stream>* stream,
                          TransportDeadline deadline) {
    beast::get_lowest_layer(*stream).expires_at(deadline);
}
#endif

std::string ComposeRequestTarget(const ParsedHttpUrl& parsed_url, const HttpRequestSpec& request) {
    const std::string query = BuildHttpQueryString(request.query_parameters);
    if (parsed_url.base_path.empty() || parsed_url.base_path == "/") {
        return request.target_path + query;
    }
    if (parsed_url.base_path.back() == '/') {
        return parsed_url.base_path.substr(0, parsed_url.base_path.size() - 1U) +
               request.target_path + query;
    }
    return parsed_url.base_path + request.target_path + query;
}

absl::StatusOr<tcp::resolver::results_type> ResolveHostWithDeadline(asio::io_context* io_context,
                                                                    const ParsedHttpUrl& parsed_url,
                                                                    TransportDeadline deadline) {
    io_context->restart();

    tcp::resolver resolver(*io_context);
    asio::steady_timer timer(*io_context);
    std::optional<tcp::resolver::results_type> endpoints;
    boost::system::error_code resolve_error;
    bool resolve_completed = false;
    bool timed_out = false;

    timer.expires_at(deadline);
    timer.async_wait(
        [&resolver, &resolve_completed, &timed_out](const boost::system::error_code& error) {
            if (error == asio::error::operation_aborted || resolve_completed) {
                return;
            }
            timed_out = true;
            resolver.cancel();
        });

    resolver.async_resolve(parsed_url.host, std::to_string(parsed_url.port),
                           [&timer, &endpoints, &resolve_error,
                            &resolve_completed](const boost::system::error_code& error,
                                                tcp::resolver::results_type results) {
                               resolve_completed = true;
                               resolve_error = error;
                               if (!error) {
                                   endpoints = std::move(results);
                               }
                               timer.cancel();
                           });

    io_context->run();
    io_context->restart();

    if (timed_out) {
        return absl::DeadlineExceededError("HTTP request timed out during DNS resolution");
    }
    if (!resolve_completed) {
        return internal_error("HTTP DNS resolution did not complete");
    }
    if (resolve_error) {
        return UnavailableTransportError("failed to resolve HTTP host", resolve_error);
    }
    if (!endpoints.has_value()) {
        return internal_error("HTTP DNS resolution returned no endpoints");
    }
    return std::move(*endpoints);
}

template <typename Stream>
absl::Status ConnectStream(asio::io_context* io_context, Stream* stream,
                           const tcp::resolver::results_type& endpoints,
                           TransportDeadline deadline) {
    io_context->restart();

    boost::system::error_code connect_error;
    bool connect_completed = false;
    SetTransportDeadline(stream, deadline);
    auto on_connect = [&connect_error, &connect_completed](const boost::system::error_code& error,
                                                           const tcp::endpoint&) {
        connect_error = error;
        connect_completed = true;
    };

    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
        stream->async_connect(endpoints, on_connect);
    }
#if !defined(_WIN32)
    else {
        beast::get_lowest_layer(*stream).async_connect(endpoints, on_connect);
    }
#endif

    io_context->run();
    io_context->restart();

    if (!connect_completed) {
        return internal_error("HTTP TCP connect did not complete");
    }
    if (connect_error) {
        return UnavailableTransportError("failed to connect to HTTP host", connect_error);
    }
    return absl::OkStatus();
}

template <typename Stream>
absl::StatusOr<HttpResponse>
ExecutePreparedRequest(asio::io_context* io_context, Stream* stream, const HttpClientConfig& config,
                       const ParsedHttpUrl& parsed_url, const HttpRequestSpec& request,
                       TransportDeadline deadline) {
    http::request<http::string_body> http_request(request.method,
                                                  ComposeRequestTarget(parsed_url, request), 11);
    http_request.set(http::field::host, parsed_url.host);
    http_request.set(http::field::user_agent, config.user_agent);
    for (const auto& [header_name, header_value] : request.headers) {
        http_request.set(header_name, header_value);
    }
    if (request.body.has_value()) {
        http_request.set(http::field::content_type, "application/json");
        http_request.body() = *request.body;
    }
    http_request.prepare_payload();
    http_request.keep_alive(false);

    io_context->restart();
    boost::system::error_code write_error;
    bool write_completed = false;
    SetTransportDeadline(stream, deadline);
    http::async_write(
        *stream, http_request,
        [&write_error, &write_completed](const boost::system::error_code& error, std::size_t) {
            write_error = error;
            write_completed = true;
        });
    io_context->run();
    io_context->restart();
    if (!write_completed) {
        return internal_error("HTTP request write did not complete");
    }
    if (write_error) {
        return UnavailableTransportError("failed to write HTTP request", write_error);
    }

    beast::flat_buffer response_buffer;
    http::response_parser<http::string_body> parser;
    parser.header_limit(kMaxResponseHeaderBytes);
    parser.body_limit(kMaxResponseBodyBytes);

    io_context->restart();
    boost::system::error_code read_error;
    bool read_completed = false;
    SetTransportDeadline(stream, deadline);
    http::async_read(
        *stream, response_buffer, parser,
        [&read_error, &read_completed](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            read_completed = true;
        });
    io_context->run();
    io_context->restart();

    if (!read_completed) {
        return internal_error("HTTP response read did not complete");
    }
    if (read_error == http::error::header_limit) {
        return absl::ResourceExhaustedError("HTTP response header exceeds maximum length");
    }
    if (read_error == http::error::body_limit) {
        return absl::ResourceExhaustedError("HTTP response body exceeds maximum length");
    }
    if (read_error) {
        return UnavailableTransportError("failed to read HTTP response", read_error);
    }

    const http::response<http::string_body> response = parser.release();
    return HttpResponse{
        .status_code = static_cast<unsigned int>(response.result_int()),
        .body = response.body(),
    };
}

#if !defined(_WIN32)
absl::Status ConfigureTlsContext(const HttpClientConfig& config, ssl::context* context) {
    boost::system::error_code error;
    context->set_default_verify_paths(error);
    if (error) {
        return UnavailableTransportError("failed to load system TLS trust store", error);
    }
    context->set_verify_mode(ssl::verify_peer);
    if (!config.trusted_ca_cert_pem.has_value() || config.trusted_ca_cert_pem->empty()) {
        return absl::OkStatus();
    }
    context->add_certificate_authority(asio::buffer(*config.trusted_ca_cert_pem), error);
    if (error) {
        return UnavailableTransportError("failed to load additional TLS CA", error);
    }
    return absl::OkStatus();
}

absl::Status ConfigureTlsStream(const ParsedHttpUrl& parsed_url,
                                beast::ssl_stream<beast::tcp_stream>* stream) {
    if (!SSL_set_tlsext_host_name(stream->native_handle(), parsed_url.host.c_str())) {
        return absl::UnavailableError("failed to configure TLS SNI host");
    }
    stream->set_verify_callback(ssl::host_name_verification(parsed_url.host));
    return absl::OkStatus();
}

absl::Status CompleteTlsHandshake(asio::io_context* io_context,
                                  beast::ssl_stream<beast::tcp_stream>* stream,
                                  TransportDeadline deadline) {
    io_context->restart();
    boost::system::error_code handshake_error;
    bool handshake_completed = false;
    SetTransportDeadline(stream, deadline);
    stream->async_handshake(ssl::stream_base::client, [&handshake_error, &handshake_completed](
                                                          const boost::system::error_code& error) {
        handshake_error = error;
        handshake_completed = true;
    });
    io_context->run();
    io_context->restart();

    if (!handshake_completed) {
        return internal_error("TLS handshake did not complete");
    }
    if (handshake_error) {
        return UnavailableTransportError("failed to complete TLS handshake", handshake_error);
    }
    return absl::OkStatus();
}
#endif

void CloseStream(beast::tcp_stream* stream) {
    boost::system::error_code ignored;
    stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
}

#if !defined(_WIN32)
void CloseStream(beast::ssl_stream<beast::tcp_stream>* stream) {
    boost::system::error_code ignored;
    stream->shutdown(ignored);
    beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(*stream).socket().close(ignored);
}
#endif

} // namespace

absl::StatusOr<ParsedHttpUrl> ParseHttpUrl(std::string_view url, std::string_view url_label) {
    const std::size_t scheme_delimiter = url.find("://");
    if (scheme_delimiter == std::string_view::npos) {
        return absl::InvalidArgumentError(std::string(url_label) +
                                          " must include a scheme such as https://");
    }

    ParsedHttpUrl parsed;
    parsed.scheme = std::string(url.substr(0, scheme_delimiter));
    std::string_view remainder = url.substr(scheme_delimiter + 3U);
    const std::size_t path_begin = remainder.find('/');
    const std::string_view authority =
        path_begin == std::string_view::npos ? remainder : remainder.substr(0, path_begin);
    parsed.base_path = path_begin == std::string_view::npos
                           ? std::string()
                           : std::string(remainder.substr(path_begin));
    if (authority.empty()) {
        return absl::InvalidArgumentError(std::string(url_label) + " must include a host");
    }
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        return absl::InvalidArgumentError(std::string(url_label) +
                                          " scheme must be 'http' or 'https'");
    }

    const std::size_t port_delimiter = authority.rfind(':');
    if (port_delimiter != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        parsed.host = std::string(authority.substr(0, port_delimiter));
        const std::string port_text(authority.substr(port_delimiter + 1U));
        if (port_text.empty()) {
            return absl::InvalidArgumentError(std::string(url_label) + " port must not be empty");
        }
        int port = 0;
        try {
            port = std::stoi(port_text);
        } catch (const std::exception&) {
            return absl::InvalidArgumentError(std::string(url_label) +
                                              " port must be a base-10 integer");
        }
        if (port < 0 || port > 65535) {
            return absl::InvalidArgumentError(std::string(url_label) +
                                              " port must be between 0 and 65535");
        }
        parsed.port = static_cast<std::uint16_t>(port);
    } else {
        parsed.host = std::string(authority);
        parsed.port = parsed.scheme == "https" ? 443 : 80;
    }

    if (parsed.host.empty()) {
        return absl::InvalidArgumentError(std::string(url_label) + " must include a host");
    }
    return parsed;
}

std::string
BuildHttpQueryString(const std::vector<std::pair<std::string, std::string>>& query_parameters) {
    if (query_parameters.empty()) {
        return "";
    }
    std::string result = "?";
    for (std::size_t i = 0; i < query_parameters.size(); ++i) {
        if (i > 0U) {
            result.push_back('&');
        }
        result += PercentEncode(query_parameters[i].first);
        result.push_back('=');
        result += PercentEncode(query_parameters[i].second);
    }
    return result;
}

absl::StatusOr<HttpResponse> ExecuteHttpRequest(const ParsedHttpUrl& parsed_url,
                                                const HttpClientConfig& config,
                                                const HttpRequestSpec& request) {
    const TransportDeadline deadline = ComputeTransportDeadline(config);
    asio::io_context io_context;

    const absl::StatusOr<tcp::resolver::results_type> endpoints =
        ResolveHostWithDeadline(&io_context, parsed_url, deadline);
    if (!endpoints.ok()) {
        return endpoints.status();
    }

    if (parsed_url.scheme == "http") {
        beast::tcp_stream stream(io_context);
        if (absl::Status status = ConnectStream(&io_context, &stream, *endpoints, deadline);
            !status.ok()) {
            return status;
        }
        const absl::StatusOr<HttpResponse> response =
            ExecutePreparedRequest(&io_context, &stream, config, parsed_url, request, deadline);
        CloseStream(&stream);
        return response;
    }

#if defined(_WIN32)
    return absl::FailedPreconditionError(
        "https transport is unavailable in Windows builds; run the gateway server on Linux");
#else
    ssl::context context(ssl::context::tls_client);
    if (absl::Status status = ConfigureTlsContext(config, &context); !status.ok()) {
        return status;
    }
    beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
    if (absl::Status status = ConfigureTlsStream(parsed_url, &stream); !status.ok()) {
        return status;
    }
    if (absl::Status status = ConnectStream(&io_context, &stream, *endpoints, deadline);
        !status.ok()) {
        return status;
    }
    if (absl::Status status = CompleteTlsHandshake(&io_context, &stream, deadline); !status.ok()) {
        CloseStream(&stream);
        return status;
    }
    const absl::StatusOr<HttpResponse> response =
        ExecutePreparedRequest(&io_context, &stream, config, parsed_url, request, deadline);
    CloseStream(&stream);
    return response;
#endif
}

} // namespace isla::server
