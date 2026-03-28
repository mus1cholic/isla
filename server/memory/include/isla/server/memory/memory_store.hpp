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

struct UserWorkingMemoryRecord {
    std::string user_id;
    std::string session_id;
    WorkingMemoryState working_memory;
    std::string rendered_working_memory;
    Timestamp updated_at;
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

struct EntityWrite {
    Entity entity;
};

struct RelationshipWrite {
    Relationship relationship;
};

struct LongTermEpisodeWrite {
    LongTermEpisode episode;
};

struct LongTermEpisodeEntityLink {
    std::string lte_id;
    std::vector<std::string> entity_ids;
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

// Validates the top-level persisted session row before it is written to a store.
[[nodiscard]] absl::Status ValidateMemorySessionRecord(const MemorySessionRecord& record);

// Validates the current user-scoped working-memory snapshot before it is written to a store.
[[nodiscard]] absl::Status ValidateUserWorkingMemoryRecord(const UserWorkingMemoryRecord& record);

// Validates an appended conversation message write, including ordering indices and required ids.
[[nodiscard]] absl::Status ValidateConversationMessageWrite(const ConversationMessageWrite& write);

// Validates a replacement write that converts a conversation item into an episode stub.
[[nodiscard]] absl::Status ValidateEpisodeStubWrite(const EpisodeStubWrite& write);

// Validates the persisted representation of a completed mid-term episode.
[[nodiscard]] absl::Status ValidateMidTermEpisodeWrite(const MidTermEpisodeWrite& write);

// Validates a split-flush write that replaces one item with a stub and preserves the remaining
// live messages as a new ongoing episode.
[[nodiscard]] absl::Status ValidateSplitEpisodeStubWrite(const SplitEpisodeStubWrite& write);

// Validates a fully loaded snapshot before it is rehydrated into working-memory state.
[[nodiscard]] absl::Status ValidateMemoryStoreSnapshot(const MemoryStoreSnapshot& snapshot);

// Validates an entity write before it is persisted to the long-term Knowledge Graph.
[[nodiscard]] absl::Status ValidateEntityWrite(const EntityWrite& write);

// Validates a relationship write before it is persisted to the long-term Knowledge Graph.
[[nodiscard]] absl::Status ValidateRelationshipWrite(const RelationshipWrite& write);

// Validates a long-term episode write before it is persisted to episodic memory.
[[nodiscard]] absl::Status ValidateLongTermEpisodeWrite(const LongTermEpisodeWrite& write);

// Validates a long-term episode entity link before it is persisted to the junction table.
[[nodiscard]] absl::Status ValidateLongTermEpisodeEntityLink(const LongTermEpisodeEntityLink& link);

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

    // Creates or updates the persisted session row for the current session id.
    [[nodiscard]] virtual absl::Status UpsertSession(const MemorySessionRecord& record) = 0;

    // Creates or updates the persisted current working-memory snapshot for one user.
    // Callers must ensure the corresponding session row already exists (for example via
    // UpsertSession or MemoryOrchestrator::BeginSession) before invoking this. The Supabase schema
    // intentionally enforces that dependency with a foreign key from
    // user_working_memory.session_id to memory_sessions.session_id.
    [[nodiscard]] virtual absl::Status
    UpsertUserWorkingMemory(const UserWorkingMemoryRecord& record) = 0;

    // Appends a single message into the persisted conversation timeline for an ongoing episode.
    [[nodiscard]] virtual absl::Status
    AppendConversationMessage(const ConversationMessageWrite& write) = 0;

    // Replaces an entire persisted conversation item with an episode stub after a full flush.
    [[nodiscard]] virtual absl::Status
    ReplaceConversationItemWithEpisodeStub(const EpisodeStubWrite& write) = 0;

    // Creates or updates the persisted mid-term episode generated from a flushed conversation
    // segment.
    [[nodiscard]] virtual absl::Status UpsertMidTermEpisode(const MidTermEpisodeWrite& write) = 0;

    // Applies a split flush by replacing the completed prefix with a stub and retaining the
    // unfinished tail as a new ongoing episode.
    [[nodiscard]] virtual absl::Status
    SplitConversationItemWithEpisodeStub(const SplitEpisodeStubWrite& write) = 0;

    // Clears the persisted working-set surfaces for one session so the next wake cycle starts with
    // an empty live conversation and no mid-term staging data. The session row and the
    // user-scoped working-memory snapshot are preserved separately.
    [[nodiscard]] virtual absl::Status ClearSessionWorkingSet(std::string_view session_id) = 0;

    // Lists all persisted mid-term episodes for the session.
    [[nodiscard]] virtual absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view session_id) const = 0;

    // Returns one persisted mid-term episode when present.
    [[nodiscard]] virtual absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const = 0;

    // Loads the persisted session, conversation items, and mid-term episodes as one snapshot.
    [[nodiscard]] virtual absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const = 0;

    // -------------------------------------------------------------------------
    // Long-term memory (Knowledge Graph + Episodic Vector Store)
    // -------------------------------------------------------------------------

    // Creates or updates a persisted entity in the Knowledge Graph.
    [[nodiscard]] virtual absl::Status UpsertEntity(const EntityWrite& write) = 0;

    // Creates or updates a persisted relationship in the Knowledge Graph.
    [[nodiscard]] virtual absl::Status UpsertRelationship(const RelationshipWrite& write) = 0;

    // Creates or updates a persisted long-term episode in the episodic vector store.
    [[nodiscard]] virtual absl::Status UpsertLongTermEpisode(const LongTermEpisodeWrite& write) = 0;

    // Writes the entity cross-links for a long-term episode into the junction table.
    [[nodiscard]] virtual absl::Status
    LinkLongTermEpisodeEntities(const LongTermEpisodeEntityLink& link) = 0;

    // Lists all persisted entities for a user.
    [[nodiscard]] virtual absl::StatusOr<std::vector<Entity>>
    ListEntitiesByUser(std::string_view user_id) const = 0;

    // Returns one persisted entity when present.
    [[nodiscard]] virtual absl::StatusOr<std::optional<Entity>>
    GetEntity(std::string_view entity_id) const = 0;

    // Lists all non-archived relationships originating from an entity.
    [[nodiscard]] virtual absl::StatusOr<std::vector<Relationship>>
    ListRelationshipsForEntity(std::string_view entity_id) const = 0;

    // Lists all long-term episodes cross-linked to an entity via the junction table.
    [[nodiscard]] virtual absl::StatusOr<std::vector<LongTermEpisode>>
    ListLongTermEpisodesForEntity(std::string_view entity_id) const = 0;
};

using MemoryStorePtr = std::shared_ptr<MemoryStore>;

} // namespace isla::server::memory
