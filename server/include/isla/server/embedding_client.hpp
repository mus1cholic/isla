#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server {

// Single embedding request. The same struct is used for both write-path embeddings
// (mid-term episode summaries, long-term relationship edges) and read-path embeddings
// (user query vectors for similarity search), so implementations must be safe to call
// from any thread.
struct EmbeddingRequest {
    // Provider-specific model identifier (e.g. "gemini-embedding-2-preview").
    std::string model;
    // Raw text to embed. Callers are responsible for any normalization/truncation;
    // providers will typically reject inputs that exceed their max token budget.
    std::string text;
    // When set, requests the provider produce an embedding with exactly this many
    // dimensions. Only honored by providers that support Matryoshka-style truncation;
    // ignored otherwise.
    std::optional<std::size_t> output_dimensionality;
    // Optional per-turn telemetry context used to attribute latency and token usage
    // back to a specific user turn. Pass nullptr for background work (sleep cycle
    // consolidation, backfills) that is not tied to a live turn.
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

// Provider-agnostic interface for text embedding models. Implementations wrap a
// concrete backend (Gemini, OpenAI, a local model, a test stub) and are shared
// across the orchestrator so the same client instance serves both write and read
// paths. All methods must be thread-safe.
class EmbeddingClient {
  public:
    virtual ~EmbeddingClient() = default;

    // Cheap readiness check used at startup to verify credentials/endpoints are
    // reachable and the configured model is available. Returns OK if the client
    // is ready to accept Embed() calls; errors surface to the caller so startup
    // can fail fast with a descriptive reason.
    [[nodiscard]] virtual absl::Status Validate() const = 0;

    // Eagerly establishes reusable transport state (TLS handshakes, connection
    // pools, DNS lookups) when the provider supports warmup so the first Embed()
    // call avoids connection-setup latency. Default: no-op.
    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    // Embeds `request.text` using the provider identified by `request.model` and
    // returns the resulting vector. Must be thread-safe; callers may issue
    // concurrent Embed() calls from retrieval and consolidation paths. Returns a
    // non-OK status for transport errors, provider rejections, and dimensionality
    // mismatches — callers should log and fall back rather than propagate failures
    // up the turn pipeline.
    [[nodiscard]] virtual absl::StatusOr<isla::server::memory::Embedding>
    Embed(const EmbeddingRequest& request) const = 0;
};

} // namespace isla::server
