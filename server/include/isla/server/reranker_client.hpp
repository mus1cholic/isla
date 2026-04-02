#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server {

struct RerankRequest {
    std::string model;
    std::string query;
    std::vector<std::string> candidates;
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

class RerankerClient {
  public:
    virtual ~RerankerClient() = default;

    [[nodiscard]] virtual absl::Status Validate() const = 0;

    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    [[nodiscard]] virtual absl::StatusOr<std::vector<double>>
    Rerank(const RerankRequest& request) const = 0;
};

} // namespace isla::server
