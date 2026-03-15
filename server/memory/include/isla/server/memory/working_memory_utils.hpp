#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

void UpsertActiveModel(PersistentMemoryCache& cache, std::string entity_id, std::string text);
void UpsertFamiliarLabel(PersistentMemoryCache& cache, std::string entity_id, std::string text);
[[nodiscard]] std::string EscapePromptText(std::string_view text);

void AppendPersistentMemoryCacheSection(std::string& output, const PersistentMemoryCache& cache);

void AppendMidTermEpisodesSection(std::string& output,
                                  const std::vector<Episode>& mid_term_episodes);
void AppendRetrievedMemorySection(std::string& output,
                                  const std::optional<RetrievedMemory>& retrieved_memory);
absl::Status AppendConversationSection(std::string& output, const Conversation& conversation);

[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryContext(const WorkingMemoryState& working_memory);

[[nodiscard]] absl::StatusOr<std::string>
RenderWorkingMemoryPrompt(const WorkingMemoryState& working_memory);

} // namespace isla::server::memory
