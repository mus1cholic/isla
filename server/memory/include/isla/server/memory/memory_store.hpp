#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct MemorySessionRecord {
    std::string session_id;
    std::string user_id;
    std::string system_prompt;
    Timestamp created_at;
    std::optional<Timestamp> ended_at;
};

struct ConversationMessageWrite {
    std::string session_id;
    std::int64_t conversation_item_index = 0;
    std::int64_t message_index = 0;
    std::string turn_id;
    MessageRole role = MessageRole::User;
    std::string content;
    Timestamp create_time;
};

struct EpisodeStubWrite {
    std::string session_id;
    std::int64_t conversation_item_index = 0;
    std::string episode_id;
    std::string episode_stub_content;
    Timestamp episode_stub_create_time;
};

struct MidTermEpisodeWrite {
    std::string session_id;
    std::int64_t source_conversation_item_index = 0;
    Episode episode;
};

struct SplitEpisodeStubWrite {
    std::string session_id;
    std::int64_t conversation_item_index = 0;
    std::string episode_id;
    std::string episode_stub_content;
    Timestamp episode_stub_create_time;
    OngoingEpisode remaining_ongoing_episode;
};

struct PersistedConversationItem {
    std::int64_t conversation_item_index = 0;
    ConversationItemType type = ConversationItemType::OngoingEpisode;
    std::optional<OngoingEpisode> ongoing_episode;
    std::optional<EpisodeStub> episode_stub;
    std::optional<std::string> episode_id;
};

struct MemoryStoreSnapshot {
    MemorySessionRecord session;
    std::vector<PersistedConversationItem> conversation_items;
    std::vector<Episode> mid_term_episodes;
};

[[nodiscard]] absl::Status ValidateMemorySessionRecord(const MemorySessionRecord& record);
[[nodiscard]] absl::Status ValidateConversationMessageWrite(const ConversationMessageWrite& write);
[[nodiscard]] absl::Status ValidateEpisodeStubWrite(const EpisodeStubWrite& write);
[[nodiscard]] absl::Status ValidateMidTermEpisodeWrite(const MidTermEpisodeWrite& write);
[[nodiscard]] absl::Status ValidateSplitEpisodeStubWrite(const SplitEpisodeStubWrite& write);
[[nodiscard]] absl::Status ValidateMemoryStoreSnapshot(const MemoryStoreSnapshot& snapshot);

class MemoryStore {
  public:
    virtual ~MemoryStore() = default;

    // Eagerly establishes the underlying transport connection so that the first
    // data operation does not pay the connection-setup latency. Default
    // implementation is a no-op for stores that connect lazily. Safe to call
    // multiple times.
    [[nodiscard]] virtual absl::Status WarmUp() {
        return absl::OkStatus();
    }

    [[nodiscard]] virtual absl::Status UpsertSession(const MemorySessionRecord& record) = 0;
    [[nodiscard]] virtual absl::Status
    AppendConversationMessage(const ConversationMessageWrite& write) = 0;
    [[nodiscard]] virtual absl::Status
    ReplaceConversationItemWithEpisodeStub(const EpisodeStubWrite& write) = 0;
    [[nodiscard]] virtual absl::Status UpsertMidTermEpisode(const MidTermEpisodeWrite& write) = 0;
    [[nodiscard]] virtual absl::Status
    SplitConversationItemWithEpisodeStub(const SplitEpisodeStubWrite& write) = 0;
    [[nodiscard]] virtual absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view session_id) const = 0;
    [[nodiscard]] virtual absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const = 0;
    [[nodiscard]] virtual absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const = 0;
};

using MemoryStorePtr = std::shared_ptr<MemoryStore>;

} // namespace isla::server::memory
