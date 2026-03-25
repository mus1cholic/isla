#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/llm_client.hpp"

namespace isla::server {

struct OllamaLlmClientConfig {
    bool enabled = false;
    std::string base_url = "http://127.0.0.1:11434";
    std::optional<std::string> api_key;
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(60) };
    std::string user_agent = "isla";
};

[[nodiscard]] absl::Status ValidateOllamaLlmClientConfig(const OllamaLlmClientConfig& config);

[[nodiscard]] absl::StatusOr<std::shared_ptr<const LlmClient>>
CreateOllamaLlmClient(OllamaLlmClientConfig config);

} // namespace isla::server
