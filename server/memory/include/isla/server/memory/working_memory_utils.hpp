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

// Promotes an entity into the active-model cache while removing any familiar-label duplicate.
void UpsertActiveModel(PersistentMemoryCache& cache, std::string entity_id, std::string text);

// Stores an entity in the familiar-label cache while removing any active-model duplicate.
void UpsertFamiliarLabel(PersistentMemoryCache& cache, std::string entity_id, std::string text);

// Escapes control characters so arbitrary text can be embedded safely into rendered prompt blocks.
[[nodiscard]] std::string EscapePromptText(std::string_view text);

// Appends the persistent-memory cache section using the prompt rendering format.
void AppendPersistentMemoryCacheSection(std::string& output, const PersistentMemoryCache& cache);

// Renders the system prompt, working-memory context, and concatenated full prompt together.
[[nodiscard]] absl::StatusOr<RenderedWorkingMemory>
RenderWorkingMemoryBundle(const WorkingMemoryState& working_memory);

// Renders the non-system-prompt working-memory sections: mid-term episodes, retrieved memory, and
// live conversation.
[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryContext(const WorkingMemoryState& working_memory);

// Renders the complete working-memory prompt by concatenating the system prompt and context.
[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryPrompt(const WorkingMemoryState& working_memory);

} // namespace isla::server::memory
