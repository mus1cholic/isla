#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/working_memory.hpp"

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

    [[nodiscard]] virtual absl::StatusOr<CompactedMidTermEpisode>
    Compact(const MidTermCompactionRequest& request) = 0;
};

using MidTermCompactorPtr = std::shared_ptr<MidTermCompactor>;

// TODO: Add a concrete LLM-based MidTermCompactor implementation that calls an
// LLM to produce tiered summaries, keywords, and salience scores from the
// flushed episode messages. The system prompt already exists at
// server/memory/include/prompts/mid_term_compactor_system_prompt.txt.

} // namespace isla::server::memory
