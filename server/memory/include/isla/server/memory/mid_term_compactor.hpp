#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/working_memory.hpp"

namespace isla::server {
class LlmClient;
} // namespace isla::server

namespace isla::server::memory {

struct MidTermCompactionRequest {
    std::string session_id;
    OngoingEpisodeFlushCandidate flush_candidate;
};

struct CompactedMidTermEpisode {
    std::optional<std::string> tier1_detail;
    std::string tier2_summary;
    std::string tier3_ref;
    std::vector<std::string> tier3_keywords;
    int salience = 0;
    Embedding embedding;
};

class MidTermCompactor {
  public:
    virtual ~MidTermCompactor() = default;

    // Compacts a flushed ongoing episode into the mid-term representation. Implementations are
    // expected to produce non-empty Tier 2 and Tier 3 text, with Tier 1 detail optional.
    [[nodiscard]] virtual absl::StatusOr<CompactedMidTermEpisode>
    Compact(const MidTermCompactionRequest& request) = 0;
};

using MidTermCompactorPtr = std::shared_ptr<MidTermCompactor>;

// Creates the LLM-backed compactor. The returned implementation validates the model's JSON schema
// strictly so malformed compaction output is rejected before it reaches working memory.
[[nodiscard]] absl::StatusOr<MidTermCompactorPtr>
CreateLlmMidTermCompactor(std::shared_ptr<const isla::server::LlmClient> llm_client,
                          std::string model);

} // namespace isla::server::memory
