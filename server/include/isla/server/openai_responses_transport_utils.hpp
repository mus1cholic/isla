#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"
#include "isla/server/openai_responses_sse_parser.hpp"

namespace isla::server::ai_gateway {

constexpr std::size_t kMaxOpenAiTransportBodyBytes = 256U * 1024U;
constexpr std::size_t kMaxOpenAiTransportHeaderBytes = 16U * 1024U;

struct TransportStreamResult {
    SseParseSummary parse_summary;
    std::size_t body_bytes = 0;
};

absl::Status AppendTransportBytes(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                                  std::string* body_text);

absl::StatusOr<SseFeedDisposition>
ConsumeTransportChunk(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                      IncrementalSseParser* parser, const OpenAiResponsesEventCallback& on_event,
                      std::string* body_text);

std::string BuildHttpHostHeaderValue(const OpenAiResponsesClientConfig& config);
std::string BuildRawHttpRequest(const OpenAiResponsesClientConfig& config,
                                std::string_view request_json);

} // namespace isla::server::ai_gateway
