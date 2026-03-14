#pragma once

#include <string>

#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"
#include "openai_responses_transport_utils.hpp"

namespace isla::server::ai_gateway {

absl::StatusOr<TransportStreamResult> ExecuteCurl(const OpenAiResponsesClientConfig& config,
                                                  const std::string& request_json,
                                                  const OpenAiResponsesEventCallback& on_event);

} // namespace isla::server::ai_gateway
