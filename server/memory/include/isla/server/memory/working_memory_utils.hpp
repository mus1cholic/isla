#pragma once

#include <string>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

void UpsertActiveModel(PersistentMemoryCache& cache, std::string entity_id, std::string text);
void UpsertFamiliarLabel(PersistentMemoryCache& cache, std::string entity_id, std::string text);

[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryPrompt(const WorkingMemoryState& working_memory);

} // namespace isla::server::memory
