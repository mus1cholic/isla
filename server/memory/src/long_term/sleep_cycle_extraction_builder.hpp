#pragma once

#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/sleep_cycle_semantic_extractor.hpp"

namespace isla::server {
class EmbeddingClient;
} // namespace isla::server

namespace isla::server::memory {

struct SleepCycleExtractionBuilderInput {
    std::string_view session_id;
    std::string_view user_id;
    const std::vector<Episode>* mid_term_episodes = nullptr;
    MemoryStore* store = nullptr;
    SleepCycleSemanticExtractor* semantic_extractor = nullptr;
    // Optional embedding client used to populate relationship edge embeddings during
    // consolidation. When null, relationships are persisted without embeddings and will be
    // lazily backfilled on a later sleep cycle when a client is available.
    const isla::server::EmbeddingClient* embedding_client = nullptr;
    std::string_view embedding_model;
};

[[nodiscard]] absl::StatusOr<SleepCycleExtractionResult>
BuildSleepCycleExtractionResult(const SleepCycleExtractionBuilderInput& input);

} // namespace isla::server::memory
