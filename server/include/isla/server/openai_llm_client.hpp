#pragma once

#include <memory>

#include "absl/status/statusor.h"
#include "isla/server/llm_client.hpp"

namespace isla::server::ai_gateway {
class OpenAiResponsesClient;
} // namespace isla::server::ai_gateway

namespace isla::server {

[[nodiscard]] absl::StatusOr<std::shared_ptr<const LlmClient>> CreateOpenAiLlmClient(
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> responses_client);

} // namespace isla::server
