#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct RenderedWorkingMemory {
    std::string system_prompt;
    std::string context;
    std::string full_prompt;
};

void UpsertActiveModel(PersistentMemoryCache& cache, std::string entity_id, std::string text);
void UpsertFamiliarLabel(PersistentMemoryCache& cache, std::string entity_id, std::string text);
[[nodiscard]] std::string EscapePromptText(std::string_view text);

void AppendPersistentMemoryCacheSection(std::string& output, const PersistentMemoryCache& cache);

[[nodiscard]] absl::StatusOr<RenderedWorkingMemory>
RenderWorkingMemoryBundle(const WorkingMemoryState& working_memory);

[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryContext(const WorkingMemoryState& working_memory);

[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryPrompt(const WorkingMemoryState& working_memory);

} // namespace isla::server::memory
