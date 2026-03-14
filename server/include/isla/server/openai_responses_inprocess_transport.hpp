#pragma once

#include <string>

#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"
#include "openai_responses_transport_utils.hpp"

namespace isla::server::ai_gateway {

absl::StatusOr<TransportStreamResult> ExecuteInProcessHttp(
    const OpenAiResponsesClientConfig& config, const std::string& request_json,
    const OpenAiResponsesEventCallback& on_event);

#if !defined(_WIN32)
absl::StatusOr<TransportStreamResult> ExecuteInProcessHttps(
    const OpenAiResponsesClientConfig& config, const std::string& request_json,
    const OpenAiResponsesEventCallback& on_event);
#endif

} // namespace isla::server::ai_gateway
