#include "isla/server/memory/mid_term_memory.hpp"

#include <gmock/gmock.h>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "memory_store_mock.hpp"

namespace isla::server::memory {
namespace {

using nlohmann::json;
using ::testing::_;
using ::testing::Return;
using ::testing::SaveArg;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

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
        .store = std::make_shared<test::MockMemoryStore>(),
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
    auto store = std::make_shared<test::MockMemoryStore>();
    MidTermEpisodeWrite captured_write;
    EXPECT_CALL(*store, UpsertMidTermEpisode(_))
        .WillOnce(testing::DoAll(SaveArg<0>(&captured_write), Return(absl::OkStatus())));
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::Status status =
        memory->StoreEpisode(4, MakeEpisode("ep_001", 9, Ts("2026-03-08T14:00:00Z"), "detail"));

    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(captured_write.session_id, "session_001");
    EXPECT_EQ(captured_write.source_conversation_item_index, 4);
    EXPECT_EQ(captured_write.episode.episode_id, "ep_001");
}

TEST(MidTermMemoryTest, StoreEpisodeRejectsSalienceExceedingMaximum) {
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, UpsertMidTermEpisode(_)).Times(0);
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::Status status =
        memory->StoreEpisode(0, MakeEpisode("ep_001", 11, Ts("2026-03-08T14:00:00Z")));

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MidTermMemoryTest, ListEpisodesReturnsStoreOrderedView) {
    auto store = std::make_shared<test::MockMemoryStore>();
    const std::vector<Episode> listed_episodes = {
        MakeEpisode("ep_older", 3, Ts("2026-03-08T14:00:00Z")),
        MakeEpisode("ep_newer", 8, Ts("2026-03-08T14:01:00Z"), "detail"),
    };
    EXPECT_CALL(*store, ListMidTermEpisodes("session_001")).WillOnce(Return(listed_episodes));
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::vector<Episode>> episodes = memory->ListEpisodes();

    ASSERT_TRUE(episodes.ok()) << episodes.status();
    ASSERT_EQ(episodes->size(), 2U);
    EXPECT_EQ((*episodes)[0].episode_id, "ep_older");
    EXPECT_EQ((*episodes)[1].episode_id, "ep_newer");
}

TEST(MidTermMemoryTest, ListEpisodesPropagatesStoreFailure) {
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, ListMidTermEpisodes("session_001"))
        .WillOnce(Return(absl::UnavailableError("store unavailable")));
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
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_001"))
        .WillOnce(
            Return(std::optional<Episode>(MakeEpisode("ep_001", 7, Ts("2026-03-08T14:00:00Z")))));
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    const absl::StatusOr<std::optional<Episode>> episode = memory->FindEpisode("ep_001");

    ASSERT_TRUE(episode.ok()) << episode.status();
    ASSERT_TRUE(episode->has_value());
    EXPECT_EQ(episode->value().episode_id, "ep_001");
}

TEST(MidTermMemoryTest, HasExpandableDetailReturnsTrueOnlyForEligibleEpisodes) {
    auto store = std::make_shared<test::MockMemoryStore>();
    absl::StatusOr<MidTermMemory> memory = MidTermMemory::Create(MidTermMemoryInit{
        .session_id = "session_001",
        .store = store,
    });
    ASSERT_TRUE(memory.ok()) << memory.status();

    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_001"))
        .WillOnce(Return(std::optional<Episode>(
            MakeEpisode("ep_001", 9, Ts("2026-03-08T14:00:00Z"), "detail"))));
    const absl::StatusOr<bool> expandable = memory->HasExpandableDetail("ep_001");
    ASSERT_TRUE(expandable.ok()) << expandable.status();
    EXPECT_TRUE(*expandable);

    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_002"))
        .WillOnce(
            Return(std::optional<Episode>(MakeEpisode("ep_002", 5, Ts("2026-03-08T14:01:00Z")))));
    const absl::StatusOr<bool> not_expandable = memory->HasExpandableDetail("ep_002");
    ASSERT_TRUE(not_expandable.ok()) << not_expandable.status();
    EXPECT_FALSE(*not_expandable);

    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_003"))
        .WillOnce(Return(
            std::optional<Episode>(MakeEpisode("ep_003", 8, Ts("2026-03-08T14:02:00Z"), ""))));
    const absl::StatusOr<bool> empty_detail = memory->HasExpandableDetail("ep_003");
    ASSERT_TRUE(empty_detail.ok()) << empty_detail.status();
    EXPECT_FALSE(*empty_detail);
}

TEST(MidTermMemoryTest, HasExpandableDetailRejectsMissingEpisode) {
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_missing"))
        .WillOnce(Return(std::optional<Episode>{}));
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
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_001"))
        .WillOnce(Return(std::optional<Episode>(
            MakeEpisode("ep_001", 8, Ts("2026-03-08T14:00:00Z"), "exact detail"))));
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
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_001"))
        .WillOnce(
            Return(std::optional<Episode>(MakeEpisode("ep_001", 7, Ts("2026-03-08T14:00:00Z")))));
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
    auto store = std::make_shared<test::MockMemoryStore>();
    EXPECT_CALL(*store, GetMidTermEpisode("session_001", "ep_001"))
        .WillOnce(Return(absl::UnavailableError("lookup failed")));
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
