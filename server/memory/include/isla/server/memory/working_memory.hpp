#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/memory_types.hpp"

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
};

[[nodiscard]] bool IsExpandableEpisode(const Episode& episode);

class WorkingMemory {
  public:
    explicit WorkingMemory(WorkingMemoryState state);

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

    void AppendEpisodeStub(std::string content, Timestamp create_time);
    void SetRetrievedMemory(std::optional<RetrievedMemory> retrieved_memory);

    void UpsertActiveModel(std::string entity_id, std::string text);
    void UpsertFamiliarLabel(std::string entity_id, std::string text);
    [[nodiscard]] absl::Status WriteBackCoreEntity(std::string_view entity_id, std::string text);

    [[nodiscard]] absl::StatusOr<OngoingEpisodeFlushCandidate>
    CaptureOngoingEpisodeForFlush(std::size_t conversation_item_index) const;
    [[nodiscard]] absl::Status
    ApplyCompletedOngoingEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);
    [[nodiscard]] absl::StatusOr<std::string> RenderSystemPrompt() const;
    [[nodiscard]] absl::StatusOr<std::string> RenderWorkingMemoryContext() const;
    [[nodiscard]] absl::StatusOr<std::string> RenderFullWorkingMemory() const;

  private:
    WorkingMemoryState state_;
};

} // namespace isla::server::memory
