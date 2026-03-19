#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/embedding_client.hpp"

namespace isla::server {

struct GeminiApiEmbeddingClientConfig {
    bool enabled = false;
    std::string api_key;
    std::string scheme = "https";
    std::string host = "generativelanguage.googleapis.com";
    std::uint16_t port = 443;
    std::optional<std::string> trusted_ca_cert_pem;
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(60) };
    std::string user_agent = "isla-gemini-api-embedding/phase-1";
};

[[nodiscard]] absl::Status
ValidateGeminiApiEmbeddingClientConfig(const GeminiApiEmbeddingClientConfig& config);

[[nodiscard]] absl::StatusOr<std::shared_ptr<const EmbeddingClient>>
CreateGeminiApiEmbeddingClient(GeminiApiEmbeddingClientConfig config);

} // namespace isla::server
