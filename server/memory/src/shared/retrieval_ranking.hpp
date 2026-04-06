#pragma once

#include <cstddef>
#include <vector>

#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct RetrievalRankingStats {
    std::size_t missing_embedding_count = 0;
    std::size_t malformed_embedding_count = 0;
};

// Ranks relationship edges by cosine similarity to the query embedding. Relationships without a
// usable embedding are treated as very low score but remain eligible so older rows can still be
// surfaced as a deterministic fallback.
[[nodiscard]] std::vector<Relationship>
RankEdgesBySimilarity(const Embedding& query_embedding,
                      const std::vector<Relationship>& relationships, std::size_t top_k,
                      RetrievalRankingStats* stats = nullptr);

} // namespace isla::server::memory
