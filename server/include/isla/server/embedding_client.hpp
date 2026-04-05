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
    // dimensions. Providers that support Matryoshka-style truncation (e.g. Gemini's
    // gemini-embedding-2 family) honor this directly. Implementations are required
    // to validate the returned vector against this value and return a non-OK status
    // from Embed() if it does not match — so setting this field on a provider that
    // does NOT support dimensionality control will fail the request rather than
    // silently falling back to the model's native dimensionality. Leave unset to
    // accept whatever dimensionality the model natively produces.
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
    // mismatches against `request.output_dimensionality`.
    //
    // Error-handling policy is the caller's choice and varies by call site:
    //   - Retrieval path (query-time similarity search) and sleep-cycle edge
    //     embedding both log a warning and fall back gracefully (skip the
    //     similarity hop / persist the row without an embedding for later
    //     backfill) so a provider outage never fails a live turn.
    //   - Write path during mid-term compaction currently propagates the status
    //     up so the failing turn surfaces the error; this is intentional because
    //     mid-term episodes without an embedding cannot be found by later
    //     retrieval.
    // New callers should pick explicitly based on whether the embedding is
    // load-bearing for the operation or merely an optional enrichment.
    [[nodiscard]] virtual absl::StatusOr<isla::server::memory::Embedding>
    Embed(const EmbeddingRequest& request) const = 0;
};

} // namespace isla::server
