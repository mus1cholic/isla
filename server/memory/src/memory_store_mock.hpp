#pragma once

#include <gmock/gmock.h>

#include "isla/server/memory/memory_store.hpp"

namespace isla::server::memory::test {

class MockMemoryStore : public MemoryStore {
  public:
    MOCK_METHOD(absl::Status, WarmUp, (), (override));
    MOCK_METHOD(absl::Status, UpsertSession, (const MemorySessionRecord& record), (override));
    MOCK_METHOD(absl::Status, UpsertUserWorkingMemory, (const UserWorkingMemoryRecord& record),
                (override));
    MOCK_METHOD(absl::Status, AppendConversationMessage, (const ConversationMessageWrite& write),
                (override));
    MOCK_METHOD(absl::Status, ReplaceConversationItemWithEpisodeStub,
                (const EpisodeStubWrite& write), (override));
    MOCK_METHOD(absl::Status, UpsertMidTermEpisode, (const MidTermEpisodeWrite& write), (override));
    MOCK_METHOD(absl::Status, SplitConversationItemWithEpisodeStub,
                (const SplitEpisodeStubWrite& write), (override));
    MOCK_METHOD(absl::Status, ClearSessionWorkingSet, (std::string_view session_id), (override));
    MOCK_METHOD((absl::StatusOr<std::vector<Episode>>), ListMidTermEpisodes,
                (std::string_view session_id), (const, override));
    MOCK_METHOD((absl::StatusOr<std::optional<Episode>>), GetMidTermEpisode,
                (std::string_view session_id, std::string_view episode_id), (const, override));
    MOCK_METHOD((absl::StatusOr<std::optional<MemoryStoreSnapshot>>), LoadSnapshot,
                (std::string_view session_id), (const, override));
};

} // namespace isla::server::memory::test
