#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server {

// Single rerank request. Candidates are scored against the query as a batch;
// implementations should issue a single provider call rather than fanning out
// per candidate so the cross-encoder sees the full candidate set in one pass.
struct RerankRequest {
    // Provider-specific model identifier (e.g. "jina-reranker-v2-base-multilingual").
    std::string model;
    // User query or other anchor text the candidates should be scored against.
    std::string query;
    // Candidate passages to score. Order is preserved: the returned score vector
    // is parallel to this vector (index i = score for candidates[i]).
    std::vector<std::string> candidates;
    // Optional per-turn telemetry context used to attribute latency/token usage
    // back to a specific user turn. Pass nullptr for background scoring work.
    std::shared_ptr<const ai_gateway::TurnTelemetryContext> telemetry_context;
};

// Provider-agnostic interface for cross-encoder rerankers. Used by the retrieval
// pipeline to rescore a candidate set from cheap first-pass retrieval (embedding
// similarity, KG 1-hop expansion) before handing the top-N to the prompt. All
// methods must be thread-safe.
class RerankerClient {
  public:
    virtual ~RerankerClient() = default;

    // Cheap readiness check used at startup to verify credentials/endpoints are
    // reachable and the configured model is available. Returns OK if the client
    // is ready to accept Rerank() calls.
    [[nodiscard]] virtual absl::Status Validate() const = 0;

    // Eagerly establishes reusable transport state when the provider supports
    // warmup so the first Rerank() call avoids connection-setup latency.
    // Default: no-op.
    [[nodiscard]] virtual absl::Status WarmUp() const {
        return absl::OkStatus();
    }

    // Scores each candidate in `request.candidates` against `request.query` and
    // returns a vector of relevance scores in the same order as the input
    // candidates (higher = more relevant). The returned vector always has the
    // same length as `request.candidates` on success. Returns a non-OK status
    // for transport errors or provider rejections; callers should fall back to
    // the pre-rerank ordering rather than failing the turn.
    [[nodiscard]] virtual absl::StatusOr<std::vector<double>>
    Rerank(const RerankRequest& request) const = 0;
};

} // namespace isla::server
