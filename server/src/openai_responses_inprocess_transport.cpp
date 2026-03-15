#include "isla/server/openai_responses_inprocess_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "absl/status/status.h"
#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#endif
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#if !defined(_WIN32)
#include <boost/beast/ssl.hpp>
#endif

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_responses_http_utils.hpp"
#include "openai_responses_inprocess_transport_resolver.hpp"

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
    if (error == beast::error::timeout || error == asio::error::timed_out) {
        VLOG(1) << "AI gateway openai responses transport timeout phase='" << SanitizeForLog(prefix)
                << "' detail='" << SanitizeForLog(error.message()) << "'";
        return absl::DeadlineExceededError(std::string(prefix) + ": " + error.message());
    }
    return absl::UnavailableError(std::string(prefix) + ": " + error.message());
}

using InProcessTransportDeadline = std::chrono::steady_clock::time_point;

struct HostResolverOverrideEntry {
    std::uint64_t token = 0U;
    std::shared_ptr<const OpenAiResponsesHostResolver> resolver;
};

std::mutex g_host_resolver_mutex;
std::vector<HostResolverOverrideEntry> g_host_resolver_overrides;
std::uint64_t g_next_host_resolver_override_token = 1U;

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

absl::StatusOr<tcp::resolver::results_type>
ResolveOpenAiHostWithDeadline(asio::io_context* io_context, tcp::resolver* resolver,
                              const OpenAiResponsesClientConfig& config,
                              InProcessTransportDeadline deadline) {
    io_context->restart();

    asio::steady_timer timer(*io_context);
    std::optional<tcp::resolver::results_type> endpoints;
    boost::system::error_code resolve_error;
    bool resolve_completed = false;
    bool timed_out = false;

    timer.expires_at(deadline);
    timer.async_wait(
        [resolver, &resolve_completed, &timed_out](const boost::system::error_code& error) {
            if (error == asio::error::operation_aborted || resolve_completed) {
                return;
            }
            timed_out = true;
            resolver->cancel();
        });

    resolver->async_resolve(config.host, std::to_string(config.port),
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
        VLOG(1) << "AI gateway openai responses transport timeout phase='DNS resolution'";
        return absl::DeadlineExceededError(
            "openai responses request timed out during DNS resolution");
    }
    if (!resolve_completed) {
        return internal_error("openai responses DNS resolution did not complete");
    }
    if (resolve_error) {
        return UnavailableTransportError("failed to resolve openai responses host", resolve_error);
    }
    if (!endpoints.has_value()) {
        return internal_error("openai responses DNS resolution returned no endpoints");
    }
    return std::move(*endpoints);
}

class AsioOpenAiResponsesHostResolver final : public OpenAiResponsesHostResolver {
  public:
    [[nodiscard]] absl::StatusOr<tcp::resolver::results_type>
    Resolve(asio::io_context* io_context, const OpenAiResponsesClientConfig& config,
            InProcessTransportDeadline deadline) const override {
        tcp::resolver resolver(*io_context);
        return ResolveOpenAiHostWithDeadline(io_context, &resolver, config, deadline);
    }
};

template <typename Stream>
absl::Status ConnectInProcessStream(asio::io_context* io_context, Stream* stream,
                                    const tcp::resolver::results_type& endpoints,
                                    InProcessTransportDeadline deadline) {
    io_context->restart();

    boost::system::error_code connect_error;
    bool connect_completed = false;
    SetInProcessTransportDeadline(stream, deadline);
    auto on_connect = [&connect_error, &connect_completed](const boost::system::error_code& error,
                                                           const tcp::endpoint&) {
        connect_error = error;
        connect_completed = true;
    };

    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
        stream->async_connect(endpoints, on_connect);
    }
#if !defined(_WIN32)
    else if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>) {
        beast::get_lowest_layer(*stream).async_connect(endpoints, on_connect);
    }
#endif

    io_context->run();
    io_context->restart();

    if (!connect_completed) {
        return internal_error("openai responses TCP connect did not complete");
    }
    if (connect_error) {
        return UnavailableTransportError("failed to connect to openai responses host",
                                         connect_error);
    }

    // Disable Nagle's algorithm so request bytes are sent immediately rather
    // than waiting up to ~200ms for more data to coalesce.
    tcp::socket* tcp_socket = nullptr;
    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
        tcp_socket = &stream->socket();
    }
#if !defined(_WIN32)
    else if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>) {
        tcp_socket = &beast::get_lowest_layer(*stream).socket();
    }
#endif
    if (tcp_socket) {
        boost::system::error_code nodelay_error;
        tcp_socket->set_option(tcp::no_delay(true), nodelay_error);
        if (nodelay_error) {
            LOG(WARNING) << "AI gateway failed to set TCP_NODELAY: " << nodelay_error.message();
        }
    }

    return absl::OkStatus();
}

template <typename Stream>
absl::Status WriteInProcessRequest(asio::io_context* io_context, Stream* stream,
                                   InProcessTransportDeadline deadline,
                                   const std::string& raw_request) {
    io_context->restart();

    boost::system::error_code write_error;
    bool write_completed = false;
    SetInProcessTransportDeadline(stream, deadline);
    asio::async_write(
        *stream, asio::buffer(raw_request),
        [&write_error, &write_completed](const boost::system::error_code& error, std::size_t) {
            write_error = error;
            write_completed = true;
        });

    io_context->run();
    io_context->restart();

    if (!write_completed) {
        return internal_error("openai responses HTTP request write did not complete");
    }
    if (write_error) {
        return UnavailableTransportError("failed to write openai responses HTTP request",
                                         write_error);
    }
    return absl::OkStatus();
}

void CloseInProcessStream(beast::tcp_stream* stream) {
    boost::system::error_code ignored;
    stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
}

std::string FindHeaderValue(const beast::http::response_parser<beast::http::buffer_body>& parser,
                            std::string_view name) {
    const auto& headers = parser.get().base();
    const auto it = headers.find(beast::string_view(name.data(), name.size()));
    if (it == headers.end()) {
        return {};
    }
    return std::string(it->value());
}

// Reads one decoded body chunk through Beast's HTTP parser. Returns the number
// of decoded bytes written into |buffer|. Uses buffer_body so Beast handles
// chunked Transfer-Encoding transparently. Sets |*http_done| when the HTTP
// message is fully consumed (chunk terminator for chunked, or Content-Length
// bytes read, or EOF for indeterminate-length responses).
template <typename Stream>
absl::StatusOr<std::size_t>
ReadDecodedBodyChunk(asio::io_context* io_context, Stream* stream,
                     InProcessTransportDeadline deadline, beast::flat_buffer* read_buffer,
                     beast::http::response_parser<beast::http::buffer_body>* parser, char* buffer,
                     std::size_t buffer_size, bool* http_done, std::string_view error_prefix) {
    if (parser->is_done()) {
        *http_done = true;
        return 0U;
    }

    parser->get().body().data = buffer;
    parser->get().body().size = buffer_size;
    parser->get().body().more = true;

    io_context->restart();
    boost::system::error_code read_error;
    bool read_completed = false;
    SetInProcessTransportDeadline(stream, deadline);
    beast::http::async_read_some(
        *stream, *read_buffer, *parser,
        [&read_error, &read_completed](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            read_completed = true;
        });
    io_context->run();
    io_context->restart();

    if (!read_completed) {
        return internal_error("openai responses transport body read did not complete");
    }
    // need_buffer means the parser filled our buffer and needs more space;
    // this is normal for streaming reads — not an error.
    if (read_error && read_error != beast::http::error::need_buffer) {
        if (read_error == asio::error::eof || read_error == beast::http::error::end_of_stream) {
            // Connection closed by peer. Let the parser see EOF so it can
            // decide whether the message was complete (e.g. indeterminate-
            // length body terminated by close) or truncated (mid-chunk or
            // short Content-Length).
            boost::system::error_code eof_error;
            parser->put_eof(eof_error);
            if (eof_error || !parser->is_done()) {
                return UnavailableTransportError(error_prefix, read_error);
            }
            *http_done = true;
            const std::size_t decoded = buffer_size - parser->get().body().size;
            return decoded;
        }
        return UnavailableTransportError(error_prefix, read_error);
    }

    *http_done = parser->is_done();
    const std::size_t decoded = buffer_size - parser->get().body().size;
    return decoded;
}

template <typename Stream>
absl::StatusOr<TransportStreamResult>
ExecuteStreamingResponse(asio::io_context* io_context, Stream* stream,
                         const OpenAiResponsesClientConfig& config, const std::string& request_json,
                         const OpenAiResponsesEventCallback& on_event,
                         InProcessTransportDeadline deadline, bool keep_alive = false,
                         bool* request_written = nullptr, bool* server_keep_alive = nullptr) {
    TransportStreamResult result;
    const std::string raw_request = BuildRawHttpRequest(config, request_json, keep_alive);
    absl::Status write_status = WriteInProcessRequest(io_context, stream, deadline, raw_request);
    if (!write_status.ok()) {
        return write_status;
    }
    if (request_written != nullptr) {
        *request_written = true;
    }

    // Use buffer_body parser for the full response so Beast handles chunked
    // Transfer-Encoding transparently. This is critical for persistent
    // connections: after SSE [DONE] we must drain the remaining HTTP response
    // (e.g. chunk terminators) before the connection can be reused.
    beast::flat_buffer read_buffer;
    beast::http::response_parser<beast::http::buffer_body> http_parser;
    http_parser.header_limit(kMaxOpenAiTransportHeaderBytes);
    // No body_limit — AppendTransportBytes enforces our own budget
    // (kMaxOpenAiTransportBodyBytes). Beast's default (8 MB) is well above
    // our 256 KB limit, so our check fires first with the correct error.

    // Read response headers.
    {
        io_context->restart();
        boost::system::error_code read_error;
        bool read_completed = false;
        SetInProcessTransportDeadline(stream, deadline);
        beast::http::async_read_header(
            *stream, read_buffer, http_parser,
            [&read_error, &read_completed](const boost::system::error_code& error, std::size_t) {
                read_error = error;
                read_completed = true;
            });
        io_context->run();
        io_context->restart();

        if (!read_completed) {
            return internal_error("openai responses HTTP response header read did not complete");
        }
        if (read_error == beast::http::error::header_limit) {
            return absl::ResourceExhaustedError(
                "openai responses transport response header exceeds maximum length");
        }
        if (read_error) {
            return UnavailableTransportError("failed to read openai responses HTTP response header",
                                             read_error);
        }
    }

    const unsigned int status_code = http_parser.get().result_int();
    result.response_headers_at = TurnTelemetryContext::Clock::now();
    if (server_keep_alive != nullptr) {
        *server_keep_alive = http_parser.get().keep_alive();
    }
    VLOG(1) << "AI gateway openai responses received response headers host='"
            << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
            << "' status_code=" << status_code << " keep_alive="
            << (http_parser.get().keep_alive() ? "true" : "false") << " request_id='"
            << SanitizeForLog(FindHeaderValue(http_parser, "x-request-id"))
            << "' openai_processing_ms='"
            << SanitizeForLog(FindHeaderValue(http_parser, "openai-processing-ms")) << "'";

    // Read body through Beast's parser (decodes chunked encoding transparently).
    std::array<char, 4096> chunk_buffer{};
    constexpr std::string_view kBodyReadError =
        "failed to read openai responses HTTP response body";

    if (status_code != 200U) {
        // Non-200: collect the full error body.
        std::string body_text;
        for (;;) {
            bool http_done = false;
            const absl::StatusOr<std::size_t> decoded = ReadDecodedBodyChunk(
                io_context, stream, deadline, &read_buffer, &http_parser, chunk_buffer.data(),
                chunk_buffer.size(), &http_done, kBodyReadError);
            if (!decoded.ok()) {
                return decoded.status();
            }
            if (*decoded > 0U) {
                if (!result.first_body_byte_at.has_value()) {
                    result.first_body_byte_at = TurnTelemetryContext::Clock::now();
                }
                absl::Status append_status = AppendTransportBytes(
                    config, std::string_view(chunk_buffer.data(), *decoded), &body_text);
                if (!append_status.ok()) {
                    return append_status;
                }
            }
            if (http_done) {
                break;
            }
        }
        return MapHttpErrorStatus(status_code, BuildHttpErrorMessage(status_code, body_text));
    }

    // 200 OK: stream decoded body bytes through the SSE parser.
    std::string body_text;
    IncrementalSseParser sse_parser;
    bool sse_completed = false;

    for (;;) {
        bool http_done = false;
        const absl::StatusOr<std::size_t> decoded = ReadDecodedBodyChunk(
            io_context, stream, deadline, &read_buffer, &http_parser, chunk_buffer.data(),
            chunk_buffer.size(), &http_done, kBodyReadError);
        if (!decoded.ok()) {
            return decoded.status();
        }

        if (*decoded > 0U && !sse_completed) {
            if (!result.first_body_byte_at.has_value()) {
                result.first_body_byte_at = TurnTelemetryContext::Clock::now();
            }
            const absl::StatusOr<SseFeedDisposition> disposition =
                ConsumeTransportChunk(config, std::string_view(chunk_buffer.data(), *decoded),
                                      &sse_parser, on_event, &body_text);
            if (!disposition.ok()) {
                return disposition.status();
            }
            if (*disposition == SseFeedDisposition::kCompleted) {
                sse_completed = true;
                // Only drain remaining HTTP response bytes when the connection
                // will actually be reused: keep_alive must be requested AND the
                // server must not have indicated Connection: close. For one-shot
                // connections or Connection: close responses, the socket will be
                // discarded so draining is unnecessary.
                const bool should_drain =
                    keep_alive && !http_done && http_parser.get().keep_alive();
                if (!should_drain) {
                    break;
                }
            }
        }

        if (http_done) {
            break;
        }
    }

    const absl::StatusOr<SseParseSummary> parse_summary = sse_parser.Finish(on_event);
    if (!parse_summary.ok()) {
        return parse_summary.status();
    }
    result.parse_summary = *parse_summary;
    result.body_bytes = body_text.size();
    return result;
}

#if !defined(_WIN32)
absl::Status CompleteTlsHandshake(asio::io_context* io_context,
                                  beast::ssl_stream<beast::tcp_stream>* stream,
                                  InProcessTransportDeadline deadline) {
    io_context->restart();

    boost::system::error_code handshake_error;
    bool handshake_completed = false;
    SetInProcessTransportDeadline(stream, deadline);
    stream->async_handshake(ssl::stream_base::client, [&handshake_error, &handshake_completed](
                                                          const boost::system::error_code& error) {
        handshake_error = error;
        handshake_completed = true;
    });

    io_context->run();
    io_context->restart();

    if (!handshake_completed) {
        return internal_error("openai responses TLS handshake did not complete");
    }
    if (handshake_error) {
        return UnavailableTransportError("failed to complete openai responses TLS handshake",
                                         handshake_error);
    }
    return absl::OkStatus();
}
#endif

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

std::shared_ptr<const OpenAiResponsesHostResolver> GetOpenAiResponsesHostResolver() {
    std::lock_guard<std::mutex> lock(g_host_resolver_mutex);
    if (!g_host_resolver_overrides.empty()) {
        return g_host_resolver_overrides.back().resolver;
    }
    static const std::shared_ptr<const OpenAiResponsesHostResolver> kDefaultResolver =
        std::make_shared<const AsioOpenAiResponsesHostResolver>();
    return kDefaultResolver;
}

std::uint64_t RegisterOpenAiResponsesHostResolverOverrideForTest(
    std::shared_ptr<const OpenAiResponsesHostResolver> resolver) {
    std::lock_guard<std::mutex> lock(g_host_resolver_mutex);
    const std::uint64_t token = g_next_host_resolver_override_token++;
    g_host_resolver_overrides.push_back(HostResolverOverrideEntry{
        .token = token,
        .resolver = std::move(resolver),
    });
    return token;
}

void UnregisterOpenAiResponsesHostResolverOverrideForTest(std::uint64_t token) {
    std::lock_guard<std::mutex> lock(g_host_resolver_mutex);
    const auto it = std::find_if(
        g_host_resolver_overrides.begin(), g_host_resolver_overrides.end(),
        [token](const HostResolverOverrideEntry& entry) { return entry.token == token; });
    if (it != g_host_resolver_overrides.end()) {
        g_host_resolver_overrides.erase(it);
    }
}

absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttp(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                     const OpenAiResponsesEventCallback& on_event) {
    const InProcessTransportDeadline deadline = ComputeInProcessTransportDeadline(config);
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);

    const absl::StatusOr<tcp::resolver::results_type> endpoints =
        GetOpenAiResponsesHostResolver()->Resolve(&io_context, config, deadline);
    if (!endpoints.ok()) {
        return endpoints.status();
    }

    absl::Status connect_status =
        ConnectInProcessStream(&io_context, &stream, *endpoints, deadline);
    if (!connect_status.ok()) {
        return connect_status;
    }

    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&io_context, &stream, config, request_json, on_event, deadline);
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

    beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
    absl::Status stream_status = ConfigureLinuxTlsStream(config, &stream);
    if (!stream_status.ok()) {
        return stream_status;
    }

    const absl::StatusOr<tcp::resolver::results_type> endpoints =
        GetOpenAiResponsesHostResolver()->Resolve(&io_context, config, deadline);
    if (!endpoints.ok()) {
        return endpoints.status();
    }

    absl::Status connect_status =
        ConnectInProcessStream(&io_context, &stream, *endpoints, deadline);
    if (!connect_status.ok()) {
        return connect_status;
    }

    absl::Status handshake_status = CompleteTlsHandshake(&io_context, &stream, deadline);
    if (!handshake_status.ok()) {
        CloseInProcessStream(&stream);
        return handshake_status;
    }

    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&io_context, &stream, config, request_json, on_event, deadline);
    CloseInProcessStream(&stream);
    return result;
}
#endif

struct PersistentInProcessTransport::Impl {
    boost::asio::io_context io_context;
    bool connected = false;

    std::unique_ptr<boost::beast::tcp_stream> tcp_stream;
#if !defined(_WIN32)
    std::unique_ptr<boost::asio::ssl::context> ssl_context;
    std::unique_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> ssl_stream;
#endif
};

PersistentInProcessTransport::PersistentInProcessTransport(OpenAiResponsesClientConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

PersistentInProcessTransport::~PersistentInProcessTransport() {
    if (impl_->connected) {
        if (impl_->tcp_stream) {
            CloseInProcessStream(impl_->tcp_stream.get());
        }
#if !defined(_WIN32)
        if (impl_->ssl_stream) {
            CloseInProcessStream(impl_->ssl_stream.get());
        }
#endif
    }
}

absl::StatusOr<TransportStreamResult>
PersistentInProcessTransport::Execute(const std::string& request_json,
                                      const OpenAiResponsesEventCallback& on_event) {
    std::lock_guard<std::mutex> lock(mutex_);

    // First attempt: connect if needed, then execute.
    if (absl::Status status = EnsureConnected(); !status.ok()) {
        return status;
    }

    bool request_written = false;
    bool server_keep_alive = true;
    absl::StatusOr<TransportStreamResult> result =
        ExecuteOnce(request_json, on_event, &request_written, &server_keep_alive);

    if (result.ok()) {
        // Proactively disconnect when server indicates Connection: close, so the
        // next Execute() starts with a fresh connection instead of writing to a
        // half-closed socket (which on some platforms succeeds due to TCP
        // buffering, then fails on the subsequent read with garbage data).
        if (!server_keep_alive) {
            VLOG(1) << "AI gateway openai responses persistent transport proactive disconnect "
                       "host='"
                    << SanitizeForLog(config_.host)
                    << "' reason='server indicated Connection: close'";
            Disconnect();
        }
        return result;
    }

    // Only retry if the request write itself failed (stale/broken connection).
    // If the write succeeded, the server received the request and any error
    // (malformed response, SSE parse failure, timeout) is not retriable.
    if (request_written) {
        Disconnect();
        return result;
    }
    if (!absl::IsUnavailable(result.status()) && !absl::IsDeadlineExceeded(result.status())) {
        return result;
    }

    VLOG(1) << "AI gateway openai responses persistent transport retrying on stale connection "
               "host='"
            << SanitizeForLog(config_.host)
            << "' status_code=" << static_cast<int>(result.status().code()) << " detail='"
            << SanitizeForLog(result.status().message()) << "'";
    Disconnect();
    if (absl::Status status = EnsureConnected(); !status.ok()) {
        return status;
    }
    return ExecuteOnce(request_json, on_event, /*request_written=*/nullptr,
                       /*server_keep_alive=*/nullptr);
}

absl::Status PersistentInProcessTransport::EnsureConnected() {
    if (impl_->connected) {
        return absl::OkStatus();
    }

    VLOG(1) << "AI gateway openai responses persistent transport connecting host='"
            << SanitizeForLog(config_.host) << "' scheme='" << config_.scheme << "'";

    const InProcessTransportDeadline deadline = ComputeInProcessTransportDeadline(config_);

    const absl::StatusOr<tcp::resolver::results_type> endpoints =
        GetOpenAiResponsesHostResolver()->Resolve(&impl_->io_context, config_, deadline);
    if (!endpoints.ok()) {
        return endpoints.status();
    }

    if (config_.scheme == "http") {
        impl_->tcp_stream = std::make_unique<beast::tcp_stream>(impl_->io_context);
        if (absl::Status status = ConnectInProcessStream(
                &impl_->io_context, impl_->tcp_stream.get(), *endpoints, deadline);
            !status.ok()) {
            Disconnect();
            return status;
        }
        impl_->connected = true;
        return absl::OkStatus();
    }

#if defined(_WIN32)
    return absl::FailedPreconditionError(
        "persistent in-process https transport is unavailable in Windows builds");
#else
    impl_->ssl_context = std::make_unique<ssl::context>(ssl::context::tls_client);
    if (absl::Status status = ConfigureLinuxTlsContext(config_, impl_->ssl_context.get());
        !status.ok()) {
        Disconnect();
        return status;
    }
    impl_->ssl_stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(impl_->io_context,
                                                                               *impl_->ssl_context);
    if (absl::Status status = ConfigureLinuxTlsStream(config_, impl_->ssl_stream.get());
        !status.ok()) {
        Disconnect();
        return status;
    }
    if (absl::Status status = ConnectInProcessStream(&impl_->io_context, impl_->ssl_stream.get(),
                                                     *endpoints, deadline);
        !status.ok()) {
        Disconnect();
        return status;
    }
    if (absl::Status status =
            CompleteTlsHandshake(&impl_->io_context, impl_->ssl_stream.get(), deadline);
        !status.ok()) {
        Disconnect();
        return status;
    }
    impl_->connected = true;
    return absl::OkStatus();
#endif
}

void PersistentInProcessTransport::Disconnect() {
    if (impl_->tcp_stream) {
        CloseInProcessStream(impl_->tcp_stream.get());
        impl_->tcp_stream.reset();
    }
#if !defined(_WIN32)
    if (impl_->ssl_stream) {
        CloseInProcessStream(impl_->ssl_stream.get());
        impl_->ssl_stream.reset();
    }
    impl_->ssl_context.reset();
#endif
    impl_->connected = false;
}

absl::StatusOr<TransportStreamResult>
PersistentInProcessTransport::ExecuteOnce(const std::string& request_json,
                                          const OpenAiResponsesEventCallback& on_event,
                                          bool* request_written, bool* server_keep_alive) {
    const InProcessTransportDeadline deadline = ComputeInProcessTransportDeadline(config_);

    if (impl_->tcp_stream) {
        return ExecuteStreamingResponse(&impl_->io_context, impl_->tcp_stream.get(), config_,
                                        request_json, on_event, deadline,
                                        /*keep_alive=*/true, request_written, server_keep_alive);
    }
#if !defined(_WIN32)
    if (impl_->ssl_stream) {
        return ExecuteStreamingResponse(&impl_->io_context, impl_->ssl_stream.get(), config_,
                                        request_json, on_event, deadline,
                                        /*keep_alive=*/true, request_written, server_keep_alive);
    }
#endif
    return absl::InternalError("persistent transport has no active stream");
}

} // namespace isla::server::ai_gateway
