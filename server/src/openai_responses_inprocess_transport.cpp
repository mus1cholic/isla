#include "isla/server/openai_responses_inprocess_transport.hpp"

#include <array>
#include <chrono>

#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#endif
#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#if !defined(_WIN32)
#include <boost/beast/ssl.hpp>
#endif

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_responses_http_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
#if !defined(_WIN32)
namespace ssl = asio::ssl;
#endif

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

absl::Status UnavailableTransportError(std::string_view prefix,
                                       const boost::system::error_code& error) {
    return absl::UnavailableError(std::string(prefix) + ": " + error.message());
}

struct ParsedHttpResponseHead {
    unsigned int status_code = 0;
    std::string buffered_body;
};

using InProcessTransportDeadline = std::chrono::steady_clock::time_point;

InProcessTransportDeadline
ComputeInProcessTransportDeadline(const OpenAiResponsesClientConfig& config) {
    return std::chrono::steady_clock::now() + config.request_timeout;
}

void SetInProcessTransportDeadline(beast::tcp_stream* stream, InProcessTransportDeadline deadline) {
    stream->expires_at(deadline);
}

#if !defined(_WIN32)
void SetInProcessTransportDeadline(beast::ssl_stream<beast::tcp_stream>* stream,
                                   InProcessTransportDeadline deadline) {
    beast::get_lowest_layer(*stream).expires_at(deadline);
}
#endif

template <typename Stream>
absl::StatusOr<ParsedHttpResponseHead>
ReadInProcessResponseHead(Stream* stream, InProcessTransportDeadline deadline) {
    std::string response_text;
    response_text.reserve(4096U);
    std::array<char, 4096> chunk_buffer{};
    constexpr std::string_view kHeaderTerminator = "\r\n\r\n";

    for (;;) {
        const std::size_t header_end = response_text.find(kHeaderTerminator);
        if (header_end != std::string::npos) {
            if (header_end + kHeaderTerminator.size() > kMaxOpenAiTransportHeaderBytes) {
                return absl::ResourceExhaustedError(
                    "openai responses transport response header exceeds maximum length");
            }
            break;
        }
        if (response_text.size() > kMaxOpenAiTransportHeaderBytes) {
            return absl::ResourceExhaustedError(
                "openai responses transport response header exceeds maximum length");
        }

        boost::system::error_code error;
        SetInProcessTransportDeadline(stream, deadline);
        const std::size_t bytes_read =
            stream->read_some(asio::buffer(chunk_buffer.data(), chunk_buffer.size()), error);
        if (error && error != asio::error::eof) {
            return UnavailableTransportError("failed to read openai responses HTTP response header",
                                             error);
        }
        if (bytes_read > 0U) {
            response_text.append(chunk_buffer.data(), bytes_read);
            continue;
        }
        break;
    }

    const std::size_t header_end = response_text.find(kHeaderTerminator);
    if (header_end == std::string::npos) {
        return internal_error("openai responses transport response header was incomplete");
    }

    const std::optional<unsigned int> status_code =
        ParseHttpStatusCode(response_text.substr(0, header_end + kHeaderTerminator.size()));
    if (!status_code.has_value()) {
        return internal_error(
            "openai responses transport response did not include a valid HTTP status");
    }

    return ParsedHttpResponseHead{
        .status_code = *status_code,
        .buffered_body = response_text.substr(header_end + kHeaderTerminator.size()),
    };
}

template <typename Stream>
absl::StatusOr<std::size_t>
ReadInProcessBodyChunk(Stream* stream, InProcessTransportDeadline deadline, char* buffer,
                       std::size_t buffer_size, bool* eof) {
    SetInProcessTransportDeadline(stream, deadline);
    boost::system::error_code error;
    const std::size_t bytes_read = stream->read_some(asio::buffer(buffer, buffer_size), error);
    if (error == asio::error::eof) {
        *eof = true;
        return bytes_read;
    }
    if (error) {
        return UnavailableTransportError("failed to read openai responses HTTP response body",
                                         error);
    }
    return bytes_read;
}

void CloseInProcessStream(beast::tcp_stream* stream) {
    boost::system::error_code ignored;
    stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
}

template <typename Stream>
absl::StatusOr<TransportStreamResult> ExecuteStreamingResponse(
    Stream* stream, const OpenAiResponsesClientConfig& config, const std::string& request_json,
    const OpenAiResponsesEventCallback& on_event, InProcessTransportDeadline deadline) {
    const std::string raw_request = BuildRawHttpRequest(config, request_json);
    boost::system::error_code error;
    SetInProcessTransportDeadline(stream, deadline);
    asio::write(*stream, asio::buffer(raw_request), error);
    if (error) {
        return UnavailableTransportError("failed to write openai responses HTTP request", error);
    }

    const absl::StatusOr<ParsedHttpResponseHead> response_head =
        ReadInProcessResponseHead(stream, deadline);
    if (!response_head.ok()) {
        return response_head.status();
    }

    std::string body_text;
    IncrementalSseParser parser;
    if (response_head->status_code == 200U && !response_head->buffered_body.empty()) {
        const absl::StatusOr<SseFeedDisposition> disposition = ConsumeTransportChunk(
            config, response_head->buffered_body, &parser, on_event, &body_text);
        if (!disposition.ok()) {
            return disposition.status();
        }
        if (*disposition == SseFeedDisposition::kCompleted) {
            const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
            if (!parse_summary.ok()) {
                return parse_summary.status();
            }
            return TransportStreamResult{
                .parse_summary = *parse_summary,
                .body_bytes = body_text.size(),
            };
        }
    }

    std::array<char, 4096> chunk_buffer{};
    if (response_head->status_code != 200U) {
        absl::Status append_status =
            AppendTransportBytes(config, response_head->buffered_body, &body_text);
        if (!append_status.ok()) {
            return append_status;
        }
        for (;;) {
            bool eof = false;
            const absl::StatusOr<std::size_t> bytes_read = ReadInProcessBodyChunk(
                stream, deadline, chunk_buffer.data(), chunk_buffer.size(), &eof);
            if (!bytes_read.ok()) {
                return bytes_read.status();
            }
            if (*bytes_read > 0U) {
                append_status = AppendTransportBytes(
                    config, std::string_view(chunk_buffer.data(), *bytes_read), &body_text);
                if (!append_status.ok()) {
                    return append_status;
                }
            }
            if (eof) {
                break;
            }
        }
        return MapHttpErrorStatus(response_head->status_code,
                                  BuildHttpErrorMessage(response_head->status_code, body_text));
    }

    for (;;) {
        bool eof = false;
        const absl::StatusOr<std::size_t> bytes_read = ReadInProcessBodyChunk(
            stream, deadline, chunk_buffer.data(), chunk_buffer.size(), &eof);
        if (!bytes_read.ok()) {
            return bytes_read.status();
        }

        if (*bytes_read > 0U) {
            const absl::StatusOr<SseFeedDisposition> disposition =
                ConsumeTransportChunk(config, std::string_view(chunk_buffer.data(), *bytes_read),
                                      &parser, on_event, &body_text);
            if (!disposition.ok()) {
                return disposition.status();
            }
            if (*disposition == SseFeedDisposition::kCompleted) {
                const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
                if (!parse_summary.ok()) {
                    return parse_summary.status();
                }
                return TransportStreamResult{
                    .parse_summary = *parse_summary,
                    .body_bytes = body_text.size(),
                };
            }
        }

        if (eof) {
            break;
        }
    }

    const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
    if (!parse_summary.ok()) {
        return parse_summary.status();
    }
    return TransportStreamResult{
        .parse_summary = *parse_summary,
        .body_bytes = body_text.size(),
    };
}

#if !defined(_WIN32)
absl::Status ConfigureLinuxTlsContext(ssl::context* context) {
    boost::system::error_code error;
    context->set_default_verify_paths(error);
    if (error) {
        return UnavailableTransportError("failed to load system TLS trust store", error);
    }
    context->set_verify_mode(ssl::verify_peer);
    return absl::OkStatus();
}

absl::Status ConfigureLinuxTlsContext(const OpenAiResponsesClientConfig& config,
                                      ssl::context* context) {
    absl::Status status = ConfigureLinuxTlsContext(context);
    if (!status.ok()) {
        return status;
    }
    if (!config.trusted_ca_cert_pem.has_value() || config.trusted_ca_cert_pem->empty()) {
        return absl::OkStatus();
    }

    boost::system::error_code error;
    context->add_certificate_authority(asio::buffer(*config.trusted_ca_cert_pem), error);
    if (error) {
        return UnavailableTransportError("failed to load openai responses additional TLS CA",
                                         error);
    }
    return absl::OkStatus();
}

absl::Status ConfigureLinuxTlsStream(const OpenAiResponsesClientConfig& config,
                                     beast::ssl_stream<beast::tcp_stream>* stream) {
    if (!SSL_set_tlsext_host_name(stream->native_handle(), config.host.c_str())) {
        return absl::UnavailableError("failed to configure openai responses TLS SNI host");
    }
    stream->set_verify_callback(ssl::host_name_verification(config.host));
    return absl::OkStatus();
}

void CloseInProcessStream(beast::ssl_stream<beast::tcp_stream>* stream) {
    boost::system::error_code ignored;
    stream->shutdown(ignored);
    beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(*stream).socket().close(ignored);
}
#endif

} // namespace

absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttp(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                     const OpenAiResponsesEventCallback& on_event) {
    const InProcessTransportDeadline deadline = ComputeInProcessTransportDeadline(config);
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    beast::tcp_stream stream(io_context);

    boost::system::error_code error;
    SetInProcessTransportDeadline(&stream, deadline);
    const tcp::resolver::results_type endpoints =
        resolver.resolve(config.host, std::to_string(config.port), error);
    if (error) {
        return UnavailableTransportError("failed to resolve openai responses host", error);
    }

    SetInProcessTransportDeadline(&stream, deadline);
    stream.connect(endpoints, error);
    if (error) {
        return UnavailableTransportError("failed to connect to openai responses host", error);
    }

    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&stream, config, request_json, on_event, deadline);
    CloseInProcessStream(&stream);
    return result;
}

#if !defined(_WIN32)
absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttps(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                      const OpenAiResponsesEventCallback& on_event) {
    const InProcessTransportDeadline deadline = ComputeInProcessTransportDeadline(config);
    asio::io_context io_context;
    ssl::context context(ssl::context::tls_client);
    absl::Status context_status = ConfigureLinuxTlsContext(config, &context);
    if (!context_status.ok()) {
        return context_status;
    }

    tcp::resolver resolver(io_context);
    beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
    absl::Status stream_status = ConfigureLinuxTlsStream(config, &stream);
    if (!stream_status.ok()) {
        return stream_status;
    }

    boost::system::error_code error;
    SetInProcessTransportDeadline(&stream, deadline);
    const tcp::resolver::results_type endpoints =
        resolver.resolve(config.host, std::to_string(config.port), error);
    if (error) {
        return UnavailableTransportError("failed to resolve openai responses host", error);
    }

    SetInProcessTransportDeadline(&stream, deadline);
    beast::get_lowest_layer(stream).connect(endpoints, error);
    if (error) {
        return UnavailableTransportError("failed to connect to openai responses host", error);
    }

    SetInProcessTransportDeadline(&stream, deadline);
    stream.handshake(ssl::stream_base::client, error);
    if (error) {
        CloseInProcessStream(&stream);
        return UnavailableTransportError("failed to complete openai responses TLS handshake",
                                         error);
    }

    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&stream, config, request_json, on_event, deadline);
    CloseInProcessStream(&stream);
    return result;
}
#endif

} // namespace isla::server::ai_gateway
