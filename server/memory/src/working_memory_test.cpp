#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/working_memory.hpp"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

class WorkingMemoryTest : public ::testing::Test {
  protected:
    static Timestamp Ts(std::string_view text) {
        return json(text).get<Timestamp>();
    }

    static WorkingMemory MakeMemory() {
        return WorkingMemory::Create(WorkingMemoryInit{
            .system_prompt = "You are Isla.",
            .user_id = "user_001",
        });
    }

    static void ExpectWorkingMemoryJsonEq(const WorkingMemory& memory, const json& expected) {
        EXPECT_EQ(json(memory.snapshot()).dump(2), expected.dump(2));
    }
};

TEST_F(WorkingMemoryTest, CreateBuildsEmptyWorkingMemoryShape) {
    const WorkingMemory memory = MakeMemory();

    ExpectWorkingMemoryJsonEq(memory, json::parse(R"json(
{
  "system_prompt": "You are Isla.",
  "persistent_memory_cache": {
    "active_models": [],
    "familiar_labels": []
  },
  "mid_term_episodes": [],
  "retrieved_memory": null,
  "conversation": {
    "items": [],
    "user_id": "user_001"
  }
}
)json"));
}

TEST_F(WorkingMemoryTest, RendersPromptInDocumentSectionOrder) {
    WorkingMemory memory = MakeMemory();
    memory.UpsertActiveModel("entity_user", "Airi, the user.");
    memory.UpsertFamiliarLabel("entity_mochi", "Airi's cat");
    memory.SetRetrievedMemory("Sarah prefers Thai cuisine.");
    AppendUserMessage(memory.mutable_conversation(), "Please plan Sarah's party.",
                      Ts("2026-03-08T14:01:00Z"));
    AppendAssistantMessage(memory.mutable_conversation(), "I can help with that.",
                           Ts("2026-03-08T14:01:05Z"));
    const absl::StatusOr<OngoingEpisodeFlushCandidate> captured =
        memory.CaptureOngoingEpisodeForFlush(0);
    ASSERT_TRUE(captured.ok()) << captured.status();
    ASSERT_EQ(captured->ongoing_episode.messages.size(), 2U);
    ASSERT_TRUE(memory
                    .ApplyCompletedOngoingEpisodeFlush(CompletedOngoingEpisodeFlush{
                        .conversation_item_index = 0,
                        .episode =
                            Episode{
                                .episode_id = "ep_001",
                                .tier1_detail = std::string("full detail"),
                                .tier2_summary = "Planned Sarah's party.",
                                .tier3_ref = "Party planning episode.",
                                .tier3_keywords = { "party", "sarah" },
                                .salience = 9,
                                .embedding = {},
                                .created_at = Ts("2026-03-08T14:01:10Z"),
                            },
                        .stub_timestamp = Ts("2026-03-08T14:01:11Z"),
                    })
                    .ok());

    const absl::StatusOr<std::string> prompt = memory.RenderPrompt();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    const std::size_t system_pos = prompt->find("{system_prompt}");
    const std::size_t cache_pos = prompt->find("{persistent_memory_cache}");
    const std::size_t mid_term_pos = prompt->find("{mid_term_episodes}");
    const std::size_t retrieved_pos = prompt->find("{retrieved_memory}");
    const std::size_t conversation_pos = prompt->find("{conversation}");

    ASSERT_NE(system_pos, std::string::npos);
    ASSERT_NE(cache_pos, std::string::npos);
    ASSERT_NE(mid_term_pos, std::string::npos);
    ASSERT_NE(retrieved_pos, std::string::npos);
    ASSERT_NE(conversation_pos, std::string::npos);
    EXPECT_LT(system_pos, cache_pos);
    EXPECT_LT(cache_pos, mid_term_pos);
    EXPECT_LT(mid_term_pos, retrieved_pos);
    EXPECT_LT(retrieved_pos, conversation_pos);
    EXPECT_NE(prompt->find("- [ep_001 | 2026-03-08T14:01:10Z | salience: 9 | expandable]"),
              std::string::npos);
    EXPECT_NE(prompt->find("- [stub | 2026-03-08T14:01:11Z] Party planning episode."),
              std::string::npos);

    ExpectWorkingMemoryJsonEq(memory, json::parse(R"json(
{
  "system_prompt": "You are Isla.",
  "persistent_memory_cache": {
    "active_models": [
      {
        "entity_id": "entity_user",
        "text": "Airi, the user."
      }
    ],
    "familiar_labels": [
      {
        "entity_id": "entity_mochi",
        "text": "Airi's cat"
      }
    ]
  },
  "mid_term_episodes": [
    {
      "episode_id": "ep_001",
      "tier1_detail": "full detail",
      "tier2_summary": "Planned Sarah's party.",
      "tier3_ref": "Party planning episode.",
      "tier3_keywords": ["party", "sarah"],
      "salience": 9,
      "embedding": [],
      "created_at": "2026-03-08T14:01:10Z"
    }
  ],
  "retrieved_memory": "Sarah prefers Thai cuisine.",
  "conversation": {
    "items": [
      {
        "type": "episode_stub",
        "episode_stub": {
          "content": "Party planning episode.",
          "create_time": "2026-03-08T14:01:11Z"
        }
      }
    ],
    "user_id": "user_001"
  }
}
)json"));
}

TEST_F(WorkingMemoryTest, RendersMixedConversationItemsInOriginalOrder) {
    WorkingMemory memory = MakeMemory();

    AppendUserMessage(memory.mutable_conversation(), "first", Ts("2026-03-08T14:00:01Z"));
    memory.AppendEpisodeStub("[stub]", Ts("2026-03-08T14:00:02Z"));
    BeginOngoingEpisode(memory.mutable_conversation());
    AppendAssistantMessage(memory.mutable_conversation(), "second", Ts("2026-03-08T14:00:03Z"));

    const absl::StatusOr<std::string> prompt = memory.RenderPrompt();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    const std::size_t first_pos = prompt->find("- [user | 2026-03-08T14:00:01Z] first");
    const std::size_t stub_pos = prompt->find("- [stub | 2026-03-08T14:00:02Z] [stub]");
    const std::size_t second_pos = prompt->find("- [assistant | 2026-03-08T14:00:03Z] second");

    ASSERT_NE(first_pos, std::string::npos);
    ASSERT_NE(stub_pos, std::string::npos);
    ASSERT_NE(second_pos, std::string::npos);
    EXPECT_LT(first_pos, stub_pos);
    EXPECT_LT(stub_pos, second_pos);
}

TEST_F(WorkingMemoryTest, WriteBackCoreEntityPromotesToActiveCache) {
    WorkingMemory memory = MakeMemory();
    memory.UpsertFamiliarLabel("entity_user", "Unknown user");

    ASSERT_TRUE(memory.WriteBackCoreEntity(kUserEntityId, "Airi, the user.").ok());

    const WorkingMemoryState& state = memory.snapshot();
    ASSERT_EQ(state.persistent_memory_cache.active_models.size(), 1U);
    EXPECT_EQ(state.persistent_memory_cache.active_models.front().entity_id, "entity_user");
    EXPECT_EQ(state.persistent_memory_cache.active_models.front().text, "Airi, the user.");
    EXPECT_TRUE(state.persistent_memory_cache.familiar_labels.empty());
    EXPECT_FALSE(memory.WriteBackCoreEntity("entity_sarah", "Sarah").ok());

    ExpectWorkingMemoryJsonEq(memory, json::parse(R"json(
{
  "system_prompt": "You are Isla.",
  "persistent_memory_cache": {
    "active_models": [
      {
        "entity_id": "entity_user",
        "text": "Airi, the user."
      }
    ],
    "familiar_labels": []
  },
  "mid_term_episodes": [],
  "retrieved_memory": null,
  "conversation": {
    "items": [],
    "user_id": "user_001"
  }
}
)json"));
}

TEST_F(WorkingMemoryTest, FlushOngoingEpisodeReplacesTargetEpisodeAndSortsMidTermEntries) {
    WorkingMemory memory = MakeMemory();
    AppendUserMessage(memory.mutable_conversation(), "one", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(memory.mutable_conversation(), "two", Ts("2026-03-08T14:00:02Z"));
    BeginOngoingEpisode(memory.mutable_conversation());
    AppendUserMessage(memory.mutable_conversation(), "three", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(memory.mutable_conversation(), "four", Ts("2026-03-08T14:00:04Z"));
    const absl::StatusOr<OngoingEpisodeFlushCandidate> captured_newer =
        memory.CaptureOngoingEpisodeForFlush(1);
    ASSERT_TRUE(captured_newer.ok()) << captured_newer.status();
    ASSERT_EQ(captured_newer->ongoing_episode.messages.size(), 2U);
    ASSERT_TRUE(memory
                    .ApplyCompletedOngoingEpisodeFlush(CompletedOngoingEpisodeFlush{
                        .conversation_item_index = 1,
                        .episode =
                            Episode{
                                .episode_id = "ep_newer",
                                .tier1_detail = std::nullopt,
                                .tier2_summary = "Newer summary",
                                .tier3_ref = "Newer ref",
                                .tier3_keywords = { "newer" },
                                .salience = 4,
                                .embedding = {},
                                .created_at = Ts("2026-03-08T14:00:06Z"),
                            },
                        .stub_timestamp = Ts("2026-03-08T14:00:07Z"),
                    })
                    .ok());
    const absl::StatusOr<OngoingEpisodeFlushCandidate> captured_older =
        memory.CaptureOngoingEpisodeForFlush(0);
    ASSERT_TRUE(captured_older.ok()) << captured_older.status();
    ASSERT_EQ(captured_older->ongoing_episode.messages.size(), 2U);
    ASSERT_TRUE(memory
                    .ApplyCompletedOngoingEpisodeFlush(CompletedOngoingEpisodeFlush{
                        .conversation_item_index = 0,
                        .episode =
                            Episode{
                                .episode_id = "ep_older",
                                .tier1_detail = std::nullopt,
                                .tier2_summary = "Older summary",
                                .tier3_ref = "Older ref",
                                .tier3_keywords = { "older" },
                                .salience = 3,
                                .embedding = {},
                                .created_at = Ts("2026-03-08T14:00:05Z"),
                            },
                        .stub_timestamp = Ts("2026-03-08T14:00:08Z"),
                    })
                    .ok());

    const WorkingMemoryState& state = memory.snapshot();
    ASSERT_EQ(state.conversation.items.size(), 2U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[0].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[0].episode_stub->content, "Older ref");
    EXPECT_EQ(state.conversation.items[1].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[1].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[1].episode_stub->content, "Newer ref");

    ASSERT_EQ(state.mid_term_episodes.size(), 2U);
    EXPECT_EQ(state.mid_term_episodes[0].episode_id, "ep_older");
    EXPECT_EQ(state.mid_term_episodes[1].episode_id, "ep_newer");

    ExpectWorkingMemoryJsonEq(memory, json::parse(R"json(
{
  "system_prompt": "You are Isla.",
  "persistent_memory_cache": {
    "active_models": [],
    "familiar_labels": []
  },
  "mid_term_episodes": [
    {
      "episode_id": "ep_older",
      "tier1_detail": null,
      "tier2_summary": "Older summary",
      "tier3_ref": "Older ref",
      "tier3_keywords": ["older"],
      "salience": 3,
      "embedding": [],
      "created_at": "2026-03-08T14:00:05Z"
    },
    {
      "episode_id": "ep_newer",
      "tier1_detail": null,
      "tier2_summary": "Newer summary",
      "tier3_ref": "Newer ref",
      "tier3_keywords": ["newer"],
      "salience": 4,
      "embedding": [],
      "created_at": "2026-03-08T14:00:06Z"
    }
  ],
  "retrieved_memory": null,
  "conversation": {
    "items": [
      {
        "type": "episode_stub",
        "episode_stub": {
          "content": "Older ref",
          "create_time": "2026-03-08T14:00:08Z"
        }
      },
      {
        "type": "episode_stub",
        "episode_stub": {
          "content": "Newer ref",
          "create_time": "2026-03-08T14:00:07Z"
        }
      }
    ],
    "user_id": "user_001"
  }
}
)json"));
}

TEST_F(WorkingMemoryTest, FlushOngoingEpisodePreservesNeighborConversationItems) {
    WorkingMemory memory = MakeMemory();

    AppendUserMessage(memory.mutable_conversation(), "first", Ts("2026-03-08T14:00:01Z"));
    memory.AppendEpisodeStub("[existing stub]", Ts("2026-03-08T14:00:02Z"));
    BeginOngoingEpisode(memory.mutable_conversation());
    AppendAssistantMessage(memory.mutable_conversation(), "third", Ts("2026-03-08T14:00:03Z"));
    const absl::StatusOr<OngoingEpisodeFlushCandidate> captured =
        memory.CaptureOngoingEpisodeForFlush(2);
    ASSERT_TRUE(captured.ok()) << captured.status();
    ASSERT_EQ(captured->ongoing_episode.messages.size(), 1U);

    ASSERT_TRUE(memory
                    .ApplyCompletedOngoingEpisodeFlush(CompletedOngoingEpisodeFlush{
                        .conversation_item_index = 2,
                        .episode =
                            Episode{
                                .episode_id = "ep_001",
                                .tier1_detail = std::nullopt,
                                .tier2_summary = "summary",
                                .tier3_ref = "ref",
                                .tier3_keywords = { "keyword" },
                                .salience = 2,
                                .embedding = {},
                                .created_at = Ts("2026-03-08T14:00:04Z"),
                            },
                        .stub_timestamp = Ts("2026-03-08T14:00:05Z"),
                    })
                    .ok());

    const WorkingMemoryState& state = memory.snapshot();
    ASSERT_EQ(state.conversation.items.size(), 3U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(state.conversation.items[0].ongoing_episode.has_value());
    EXPECT_EQ(state.conversation.items[0].ongoing_episode->messages[0].content, "first");
    EXPECT_EQ(state.conversation.items[1].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[1].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[1].episode_stub->content, "[existing stub]");
    EXPECT_EQ(state.conversation.items[2].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[2].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[2].episode_stub->content, "ref");
}

TEST_F(WorkingMemoryTest, RenderPromptRejectsConversationItemsMissingTaggedPayload) {
    WorkingMemory memory = MakeMemory();
    memory.mutable_conversation().items.push_back(ConversationItem{
        .type = ConversationItemType::EpisodeStub,
        .ongoing_episode = std::nullopt,
        .episode_stub = std::nullopt,
    });

    const absl::StatusOr<std::string> prompt = memory.RenderPrompt();

    ASSERT_FALSE(prompt.ok());
    EXPECT_EQ(prompt.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(WorkingMemoryTest, FlushOngoingEpisodeRejectsInvalidRequests) {
    WorkingMemory memory = MakeMemory();
    AppendUserMessage(memory.mutable_conversation(), "one", Ts("2026-03-08T14:00:01Z"));

    const absl::StatusOr<OngoingEpisodeFlushCandidate> bad_target =
        memory.CaptureOngoingEpisodeForFlush(1);
    EXPECT_FALSE(bad_target.ok());

    memory.AppendEpisodeStub("[existing stub]", Ts("2026-03-08T14:00:04Z"));
    const absl::StatusOr<OngoingEpisodeFlushCandidate> wrong_kind =
        memory.CaptureOngoingEpisodeForFlush(1);
    EXPECT_FALSE(wrong_kind.ok());

    const absl::Status bad_episode =
        memory.ApplyCompletedOngoingEpisodeFlush(CompletedOngoingEpisodeFlush{
            .conversation_item_index = 1,
            .episode =
                Episode{
                    .episode_id = "",
                    .tier1_detail = std::nullopt,
                    .tier2_summary = "",
                    .tier3_ref = "",
                    .tier3_keywords = {},
                    .salience = 1,
                    .embedding = {},
                    .created_at = Ts("2026-03-08T14:00:05Z"),
                },
            .stub_timestamp = Ts("2026-03-08T14:00:06Z"),
        });
    EXPECT_FALSE(bad_episode.ok());
}

} // namespace
} // namespace isla::server::memory
