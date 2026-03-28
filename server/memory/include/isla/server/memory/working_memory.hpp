#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/working_memory_utils.hpp"

namespace isla::server::memory {

inline constexpr std::string_view kUserEntityId = "entity_user";
inline constexpr std::string_view kAssistantEntityId = "entity_assistant";

struct WorkingMemoryInit {
    std::string system_prompt;
    std::string user_id;
};

struct OngoingEpisodeFlushCandidate {
    std::size_t conversation_item_index = 0;
    OngoingEpisode ongoing_episode;
};

struct CompletedOngoingEpisodeFlush {
    std::size_t conversation_item_index = 0;
    Episode episode;
    Timestamp stub_timestamp;
    std::optional<std::size_t> split_at_message_index;
};

class WorkingMemory {
  public:
    explicit WorkingMemory(WorkingMemoryState state);

    // Builds an empty working-memory session with the configured system prompt and user identity.
    [[nodiscard]] static absl::StatusOr<WorkingMemory> Create(const WorkingMemoryInit& init);

    [[nodiscard]] const WorkingMemoryState& snapshot() const {
        return state_;
    }

    [[nodiscard]] Conversation& mutable_conversation() {
        return state_.conversation;
    }

    [[nodiscard]] const Conversation& conversation() const {
        return state_.conversation;
    }

    // Appends a standalone episode stub to the conversation timeline.
    void AppendEpisodeStub(std::string content, Timestamp create_time);

    // Replaces the currently retrieved-memory payload used when rendering prompt context.
    void SetRetrievedMemory(std::optional<RetrievedMemory> retrieved_memory);

    // Promotes the entity into the active-model cache, removing any familiar-label entry with the
    // same id to keep the cache mutually exclusive.
    void UpsertActiveModel(std::string entity_id, std::string text);

    // Stores the entity as a familiar label, removing any active-model entry with the same id.
    void UpsertFamiliarLabel(std::string entity_id, std::string text);

    // Writes back the hardcoded core user/assistant entities into the persistent-memory cache.
    [[nodiscard]] absl::Status WriteBackCoreEntity(std::string_view entity_id, std::string text);

    // Validates that the referenced conversation item is a non-empty ongoing episode that can be
    // flushed wholesale into mid-term memory.
    [[nodiscard]] absl::Status
    ValidateOngoingEpisodeForFlush(std::size_t conversation_item_index) const;

    // Validates that the referenced ongoing episode can be split at the given user-message
    // boundary, leaving a complete completed prefix to flush and a valid tail to keep live.
    [[nodiscard]] absl::Status
    ValidateOngoingEpisodeForSplitFlush(std::size_t conversation_item_index,
                                        std::size_t split_at_message_index) const;

    // Captures a copy of the referenced ongoing episode for asynchronous flush work without
    // mutating live conversation state.
    [[nodiscard]] absl::StatusOr<OngoingEpisodeFlushCandidate>
    CaptureOngoingEpisodeForFlush(std::size_t conversation_item_index) const;

    // Captures only the completed prefix of an ongoing episode for split-flush compaction.
    [[nodiscard]] absl::StatusOr<OngoingEpisodeFlushCandidate>
    CaptureOngoingEpisodeForSplitFlush(std::size_t conversation_item_index,
                                       std::size_t split_at_message_index) const;

    // Applies a completed mid-term flush by replacing or splitting the source conversation item
    // and inserting the resulting episode into the mid-term list in timestamp order.
    [[nodiscard]] absl::Status
    ApplyCompletedOngoingEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);

    // Clears the transient working-set state at the sleep-cycle boundary while preserving the
    // system prompt and persistent-memory cache.
    void ClearForSleepCycle();

    // Renders the system prompt, context section, and concatenated full prompt in one pass.
    [[nodiscard]] absl::StatusOr<RenderedWorkingMemory> RenderPromptBundle() const;

    // Renders only the system-prompt portion of working memory.
    [[nodiscard]] absl::StatusOr<std::string> RenderSystemPrompt() const;

    // Renders only the working-memory context sections that follow the system prompt.
    [[nodiscard]] absl::StatusOr<std::string> RenderWorkingMemoryContext() const;

    // Renders the system prompt plus working-memory context as a single prompt string.
    [[nodiscard]] absl::StatusOr<std::string> RenderFullWorkingMemory() const;

  private:
    WorkingMemoryState state_;
};

} // namespace isla::server::memory
