#pragma once

#include <vector>

#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

// Ranks relationship edges by cosine similarity to the query embedding. Relationships without a
// usable embedding are treated as very low score but remain eligible so older rows can still be
// surfaced as a deterministic fallback.
[[nodiscard]] std::vector<Relationship>
RankEdgesBySimilarity(const Embedding& query_embedding,
                      const std::vector<Relationship>& relationships, int top_k);

} // namespace isla::server::memory
