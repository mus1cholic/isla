#pragma once

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server {

struct EmbeddingRequest {
    std::string model;
    std::string text;
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

class EmbeddingClient {
  public:
    virtual ~EmbeddingClient() = default;

    [[nodiscard]] virtual absl::Status Validate() const = 0;

    // Eagerly establishes reusable transport state when the provider supports
    // warmup so the first request avoids connection-setup latency.
    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    [[nodiscard]] virtual absl::StatusOr<isla::server::memory::Embedding>
    Embed(const EmbeddingRequest& request) const = 0;
};

} // namespace isla::server
