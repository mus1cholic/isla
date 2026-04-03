#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/reranker_client.hpp"

namespace isla::server {

struct JinaApiRerankerClientConfig {
    bool enabled = false;
    std::string api_key;
    std::string scheme = "https";
    std::string host = "api.jina.ai";
    std::uint16_t port = 443;
    std::string target = "/v1/rerank";
    std::optional<std::string> trusted_ca_cert_pem;
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(60) };
    std::string user_agent = "isla-jina-api-reranker";
};

[[nodiscard]] absl::Status
ValidateJinaApiRerankerClientConfig(const JinaApiRerankerClientConfig& config);

[[nodiscard]] absl::StatusOr<std::shared_ptr<const RerankerClient>>
CreateJinaApiRerankerClient(JinaApiRerankerClientConfig config);

} // namespace isla::server
