#include "isla/server/openai_responses_transport_utils.hpp"

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include <sstream>

namespace isla::server::ai_gateway {

absl::Status AppendTransportBytes(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                                  std::string* body_text) {
    body_text->append(chunk.data(), chunk.size());
    if (body_text->size() > kMaxOpenAiTransportBodyBytes) {
        LOG(ERROR) << "AI gateway openai responses transport body budget exceeded host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' body_bytes=" << body_text->size()
                   << " budget_bytes=" << kMaxOpenAiTransportBodyBytes;
        return absl::ResourceExhaustedError(
            "openai responses transport body exceeds maximum length");
    }
    return absl::OkStatus();
}

absl::StatusOr<SseFeedDisposition>
ConsumeTransportChunk(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                      IncrementalSseParser* parser, const OpenAiResponsesEventCallback& on_event,
                      std::string* body_text) {
    absl::Status append_status = AppendTransportBytes(config, chunk, body_text);
    if (!append_status.ok()) {
        return append_status;
    }

    const absl::StatusOr<SseFeedDisposition> disposition = parser->Feed(chunk, on_event);
    if (!disposition.ok()) {
        VLOG(1) << "AI gateway openai responses aborting stream on callback/parser status host='"
                << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                << "' status_code=" << static_cast<int>(disposition.status().code()) << " detail='"
                << SanitizeForLog(disposition.status().message()) << "'";
        return disposition.status();
    }
    if (*disposition == SseFeedDisposition::kCompleted) {
        VLOG(1) << "AI gateway openai responses observed terminal completion early host='"
                << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                << "' body_bytes=" << body_text->size();
    }
    return disposition;
}

std::string BuildHttpHostHeaderValue(const OpenAiResponsesClientConfig& config) {
    const bool default_http_port = config.scheme == "http" && config.port == 80;
    const bool default_https_port = config.scheme == "https" && config.port == 443;
    if (default_http_port || default_https_port) {
        return config.host;
    }
    return config.host + ":" + std::to_string(config.port);
}

std::string BuildRawHttpRequest(const OpenAiResponsesClientConfig& config,
                                std::string_view request_json) {
    std::ostringstream request;
    request << "POST " << config.target << " HTTP/1.1\r\n";
    request << "Host: " << BuildHttpHostHeaderValue(config) << "\r\n";
    request << "Authorization: Bearer " << config.api_key << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Accept: text/event-stream\r\n";
    request << "User-Agent: " << config.user_agent << "\r\n";
    if (config.organization.has_value()) {
        request << "OpenAI-Organization: " << *config.organization << "\r\n";
    }
    if (config.project.has_value()) {
        request << "OpenAI-Project: " << *config.project << "\r\n";
    }
    request << "Connection: close\r\n";
    request << "Content-Length: " << request_json.size() << "\r\n\r\n";
    request << request_json;
    return request.str();
}

} // namespace isla::server::ai_gateway
