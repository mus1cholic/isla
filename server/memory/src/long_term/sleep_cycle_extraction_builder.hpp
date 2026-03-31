#pragma once

#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/sleep_cycle_semantic_extractor.hpp"

namespace isla::server::memory {

struct SleepCycleExtractionBuilderInput {
    std::string_view session_id;
    std::string_view user_id;
    const std::vector<Episode>* mid_term_episodes = nullptr;
    MemoryStore* store = nullptr;
    SleepCycleSemanticExtractor* semantic_extractor = nullptr;
};

[[nodiscard]] absl::StatusOr<SleepCycleExtractionResult>
BuildSleepCycleExtractionResult(const SleepCycleExtractionBuilderInput& input);

} // namespace isla::server::memory
