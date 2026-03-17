#include "isla/server/memory/mid_term_memory.hpp"

#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

class RecordingMemoryStore final : public MemoryStore {
  public:
    absl::Status UpsertSession(const MemorySessionRecord& record) override {
        static_cast<void>(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(const ConversationMessageWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(const EpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(const SplitEpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status UpsertMidTermEpisode(const MidTermEpisodeWrite& write) override {
        if (!upsert_episode_status.ok()) {
            return upsert_episode_status;
        }
        episode_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view session_id) const override {
        last_list_session_id = std::string(session_id);
        if (!list_episodes_status.ok()) {
            return list_episodes_status;
        }
        return listed_episodes;
    }

    absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const override {
        last_get_session_id = std::string(session_id);
        last_get_episode_id = std::string(episode_id);
        if (!get_episode_status.ok()) {
            return get_episode_status;
        }
        return found_episode;
    }

    absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::nullopt;
    }

    mutable std::string last_list_session_id;
    mutable std::string last_get_session_id;
    mutable std::string last_get_episode_id;
    std::vector<MidTermEpisodeWrite> episode_writes;
    std::vector<Episode> listed_episodes;
    std::optional<Episode> found_episode = std::nullopt;
    absl::Status upsert_episode_status = absl::OkStatus();
    mutable absl::Status list_episodes_status = absl::OkStatus();
    mutable absl::Status get_episode_status = absl::OkStatus();
};

Episode MakeEpisode(std::string episode_id, int salience, Timestamp created_at,
                    std::optional<std::string> tier1_detail = std::nullopt) {
    return Episode{
        .episode_id = std::move(episode_id),
        .tier1_detail = std::move(tier1_detail),
        .tier2_summary = "summary",
        .tier3_ref = "ref",
        .tier3_keywords = { "memory" },
        .salience = salience,
        .embedding = {},
        .created_at = created_at,
    };
}

TEST(MidTermMemoryTest, CreateRejectsMissingSessionId) {
    const absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "",
        .store = std::make_shared<RecordingMemoryStore>(),
    });

    ASSERT_FALSE(memory.ok());
    EXPECT_EQ(memory.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(MidTermMemoryTest, CreateRejectsMissingStore) {
    const absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = nullptr,
    });

    ASSERT_FALSE(memory.ok());
    EXPECT_EQ(memory.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(MidTermMemoryTest, StoreEpisodePersistsSessionScopedWrite) {
    auto store = std::make_shared<RecordingMemoryStore>();
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::Status status =
        memory->StoreEpisode(4, MakeEpisode("ep_001", 9, Ts("2026-03-08T14:00:00Z"), "detail"));

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_EQ(store->episode_writes.size(), 1U);
    EXPECT_EQ(store->episode_writes[0].session_id, "session_001");
    EXPECT_EQ(store->episode_writes[0].source_conversation_item_index, 4);
    EXPECT_EQ(store->episode_writes[0].episode.episode_id, "ep_001");
}

TEST(MidTermMemoryTest, StoreEpisodeRejectsSalienceExceedingMaximum) {
    auto store = std::make_shared<RecordingMemoryStore>();
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::Status status =
        memory->StoreEpisode(0, MakeEpisode("ep_001", 11, Ts("2026-03-08T14:00:00Z")));

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(store->episode_writes.empty());
}

TEST(MidTermMemoryTest, ListEpisodesReturnsStoreOrderedView) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->listed_episodes = {
        MakeEpisode("ep_older", 3, Ts("2026-03-08T14:00:00Z")),
        MakeEpisode("ep_newer", 8, Ts("2026-03-08T14:01:00Z"), "detail"),
    };
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::vector<Episode>> episodes = memory->ListEpisodes();

    ASSERT_TRUE(episodes.ok()) << episodes.status();
    ASSERT_EQ(episodes->size(), 2U);
    EXPECT_EQ(store->last_list_session_id, "session_001");
    EXPECT_EQ((*episodes)[0].episode_id, "ep_older");
    EXPECT_EQ((*episodes)[1].episode_id, "ep_newer");
}

TEST(MidTermMemoryTest, ListEpisodesPropagatesStoreFailure) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->list_episodes_status = absl::UnavailableError("store unavailable");
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::vector<Episode>> episodes = memory->ListEpisodes();

    ASSERT_FALSE(episodes.ok());
    EXPECT_EQ(episodes.status().code(), absl::StatusCode::kUnavailable);
}

TEST(MidTermMemoryTest, FindEpisodeUsesSessionScopedLookup) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->found_episode = MakeEpisode("ep_001", 7, Ts("2026-03-08T14:00:00Z"));
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::optional<Episode>> episode = memory->FindEpisode("ep_001");

    ASSERT_TRUE(episode.ok()) << episode.status();
    ASSERT_TRUE(episode->has_value());
    EXPECT_EQ(store->last_get_session_id, "session_001");
    EXPECT_EQ(store->last_get_episode_id, "ep_001");
    EXPECT_EQ(episode->value().episode_id, "ep_001");
}

TEST(MidTermMemoryTest, HasExpandableDetailReturnsTrueOnlyForEligibleEpisodes) {
    auto store = std::make_shared<RecordingMemoryStore>();
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    store->found_episode = MakeEpisode("ep_001", 9, Ts("2026-03-08T14:00:00Z"), "detail");
    const absl::StatusOr<bool> expandable = memory->HasExpandableDetail("ep_001");
    ASSERT_TRUE(expandable.ok()) << expandable.status();
    EXPECT_TRUE(*expandable);

    store->found_episode = MakeEpisode("ep_002", 5, Ts("2026-03-08T14:01:00Z"));
    const absl::StatusOr<bool> not_expandable = memory->HasExpandableDetail("ep_002");
    ASSERT_TRUE(not_expandable.ok()) << not_expandable.status();
    EXPECT_FALSE(*not_expandable);

    store->found_episode = MakeEpisode("ep_003", 8, Ts("2026-03-08T14:02:00Z"), "");
    const absl::StatusOr<bool> empty_detail = memory->HasExpandableDetail("ep_003");
    ASSERT_TRUE(empty_detail.ok()) << empty_detail.status();
    EXPECT_FALSE(*empty_detail);
}

TEST(MidTermMemoryTest, HasExpandableDetailRejectsMissingEpisode) {
    auto store = std::make_shared<RecordingMemoryStore>();
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<bool> expandable = memory->HasExpandableDetail("ep_missing");

    ASSERT_FALSE(expandable.ok());
    EXPECT_EQ(expandable.status().code(), absl::StatusCode::kNotFound);
}

TEST(MidTermMemoryTest, GetExpandableDetailReturnsTier1Content) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->found_episode = MakeEpisode("ep_001", 8, Ts("2026-03-08T14:00:00Z"), "exact detail");
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::string> detail = memory->GetExpandableDetail("ep_001");

    ASSERT_TRUE(detail.ok()) << detail.status();
    EXPECT_EQ(*detail, "exact detail");
}

TEST(MidTermMemoryTest, GetExpandableDetailRejectsNonExpandableEpisode) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->found_episode = MakeEpisode("ep_001", 7, Ts("2026-03-08T14:00:00Z"));
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::string> detail = memory->GetExpandableDetail("ep_001");

    ASSERT_FALSE(detail.ok());
    EXPECT_EQ(detail.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(MidTermMemoryTest, GetExpandableDetailPropagatesLookupFailure) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->get_episode_status = absl::UnavailableError("lookup failed");
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::string> detail = memory->GetExpandableDetail("ep_001");

    ASSERT_FALSE(detail.ok());
    EXPECT_EQ(detail.status().code(), absl::StatusCode::kUnavailable);
}

} // namespace
} // namespace isla::server::memory
