#include "isla/server/memory/memory_store.hpp"

#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

TEST(MemoryStoreTest, ConversationMessageWriteRejectsMissingIdentifiers) {
    const absl::Status status = ValidateConversationMessageWrite(ConversationMessageWrite{
        .session_id = "",
        .conversation_item_index = 0,
        .message_index = 0,
        .turn_id = "",
        .role = MessageRole::User,
        .content = "",
        .create_time = Ts("2026-03-08T14:00:00Z"),
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MemoryStoreTest, MidTermEpisodeWriteRejectsOutOfRangeSalience) {
    const absl::Status status = ValidateMidTermEpisodeWrite(MidTermEpisodeWrite{
        .session_id = "session_001",
        .source_conversation_item_index = 0,
        .episode =
            Episode{
                .episode_id = "ep_001",
                .tier1_detail = std::nullopt,
                .tier2_summary = "summary",
                .tier3_ref = "ref",
                .tier3_keywords = { "memory" },
                .salience = 11,
                .embedding = {},
                .created_at = Ts("2026-03-08T14:00:00Z"),
            },
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MemoryStoreTest, EpisodeStubWriteRejectsMissingStubPayload) {
    const absl::Status status = ValidateEpisodeStubWrite(EpisodeStubWrite{
        .session_id = "session_001",
        .conversation_item_index = 0,
        .episode_id = "ep_001",
        .episode_stub_content = "",
        .episode_stub_create_time = Ts("2026-03-08T14:00:00Z"),
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MemoryStoreTest, SnapshotValidationRequiresContiguousConversationItemIndexes) {
    const absl::Status status = ValidateMemoryStoreSnapshot(MemoryStoreSnapshot{
        .session =
            MemorySessionRecord{
                .session_id = "session_001",
                .user_id = "user_001",
                .system_prompt = "You are Isla.",
                .created_at = Ts("2026-03-08T14:00:00Z"),
                .ended_at = std::nullopt,
            },
        .conversation_items =
            {
                PersistedConversationItem{
                    .conversation_item_index = 1,
                    .type = ConversationItemType::OngoingEpisode,
                    .ongoing_episode = OngoingEpisode{
                        .messages = { Message{
                            .role = MessageRole::User,
                            .content = "hello",
                            .create_time = Ts("2026-03-08T14:00:01Z"),
                        } },
                    },
                    .episode_stub = std::nullopt,
                    .episode_id = std::nullopt,
                },
            },
        .mid_term_episodes = {},
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MemoryStoreTest, SnapshotValidationAcceptsOrderedConversationAndEpisodes) {
    const absl::Status status = ValidateMemoryStoreSnapshot(MemoryStoreSnapshot{
        .session =
            MemorySessionRecord{
                .session_id = "session_001",
                .user_id = "user_001",
                .system_prompt = "You are Isla.",
                .created_at = Ts("2026-03-08T14:00:00Z"),
                .ended_at = std::nullopt,
            },
        .conversation_items =
            {
                PersistedConversationItem{
                    .conversation_item_index = 0,
                    .type = ConversationItemType::EpisodeStub,
                    .ongoing_episode = std::nullopt,
                    .episode_stub = EpisodeStub{
                        .content = "ref",
                        .create_time = Ts("2026-03-08T14:00:03Z"),
                    },
                    .episode_id = std::string("ep_001"),
                },
                PersistedConversationItem{
                    .conversation_item_index = 1,
                    .type = ConversationItemType::OngoingEpisode,
                    .ongoing_episode = OngoingEpisode{
                        .messages =
                            {
                                Message{
                                    .role = MessageRole::User,
                                    .content = "follow-up",
                                    .create_time = Ts("2026-03-08T14:00:04Z"),
                                },
                            },
                    },
                    .episode_stub = std::nullopt,
                    .episode_id = std::nullopt,
                },
            },
        .mid_term_episodes =
            {
                Episode{
                    .episode_id = "ep_001",
                    .tier1_detail = std::nullopt,
                    .tier2_summary = "summary",
                    .tier3_ref = "ref",
                    .tier3_keywords = { "memory" },
                    .salience = 7,
                    .embedding = {},
                    .created_at = Ts("2026-03-08T14:00:02Z"),
                },
            },
    });

    EXPECT_TRUE(status.ok()) << status;
}

} // namespace
} // namespace isla::server::memory
