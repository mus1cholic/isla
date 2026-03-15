#include "isla/server/memory/memory_types.hpp"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

TEST(MemoryTypesTest, ParsesSessionJsonUsingDocumentShape) {
    const json input = json::parse(R"json(
{
  "session_id": "session_001",
  "working_memory": {
    "system_prompt": {
      "base_instructions": "You are Isla.",
      "persistent_memory_cache": {
        "active_models": [
          {
            "entity_id": "entity_sarah",
            "text": "Airi's close friend."
          }
        ],
        "familiar_labels": [
          {
            "entity_id": "entity_mochi",
            "text": "Airi's cat"
          }
        ]
      }
    },
    "mid_term_episodes": [
      {
        "episode_id": "ep_UUID1",
        "tier1_detail": "raw debug log",
        "tier2_summary": "Resolved an inverted normals issue.",
        "tier3_ref": "Fixed inverted normals.",
        "tier3_keywords": ["renderer", "normals", "culling"],
        "salience": 9,
        "embedding": [0.23, 0.42],
        "created_at": "2026-03-08T14:05:00Z"
      }
    ],
    "retrieved_memory": "placeholder retrieved memory",
    "conversation": {
      "items": [
        {
          "type": "ongoing_episode",
          "ongoing_episode": {
            "messages": [
              {
                "role": "user",
                "content": "Please fix the renderer.",
                "create_time": "2026-03-08T14:00:00Z"
              }
            ]
          }
        },
        {
          "type": "episode_stub",
          "episode_stub": {
            "content": "[debugging inverted normals - resolved]",
            "create_time": "2026-03-08T14:04:00Z"
          }
        }
      ],
      "user_id": "user_001"
    }
  },
  "created_at": "2026-03-08T14:00:00Z",
  "ended_at": null
}
)json");

    const Session session = input.get<Session>();

    EXPECT_EQ(session.session_id, "session_001");
    EXPECT_EQ(session.working_memory.system_prompt.base_instructions, "You are Isla.");
    ASSERT_EQ(session.working_memory.system_prompt.persistent_memory_cache.active_models.size(), 1);
    EXPECT_EQ(session.working_memory.system_prompt.persistent_memory_cache.active_models.front()
                  .entity_id,
              "entity_sarah");
    ASSERT_EQ(session.working_memory.mid_term_episodes.size(), 1);
    EXPECT_EQ(session.working_memory.mid_term_episodes.front().salience, 9);
    ASSERT_TRUE(session.working_memory.retrieved_memory.has_value());
    EXPECT_EQ(*session.working_memory.retrieved_memory, "placeholder retrieved memory");
    ASSERT_EQ(session.working_memory.conversation.items.size(), 2);
    EXPECT_EQ(session.working_memory.conversation.items[0].type,
              ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(session.working_memory.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(session.working_memory.conversation.items[0].ongoing_episode->messages.size(), 1);
    EXPECT_EQ(session.working_memory.conversation.items[0].ongoing_episode->messages[0].role,
              MessageRole::User);
    EXPECT_EQ(session.working_memory.conversation.items[1].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(session.working_memory.conversation.items[1].episode_stub.has_value());
    EXPECT_EQ(session.working_memory.conversation.items[1].episode_stub->content,
              "[debugging inverted normals - resolved]");
    EXPECT_EQ(json(session.created_at), "2026-03-08T14:00:00Z");
    EXPECT_EQ(json(session.working_memory.mid_term_episodes.front().created_at),
              "2026-03-08T14:05:00Z");
    EXPECT_FALSE(session.ended_at.has_value());
}

TEST(MemoryTypesTest, SerializesLongTermEpisodeEnumsAndOptionalLinks) {
    const LongTermEpisode episode{
        .lte_id = "lte_UUID",
        .summary_full = "Full story",
        .summary_compressed = "Compressed story",
        .keywords = { "memory", "kg" },
        .embedding = { 0.11, 0.22 },
        .related_entities = { "entity_user", "entity_isla" },
        .outcome = LongTermEpisodeOutcome::Resolved,
        .complexity = 7,
        .created_at = json("2026-03-08T15:00:00Z").get<Timestamp>(),
        .original_episode_ids = { "ep_001", "ep_002" },
        .caused_by = std::nullopt,
        .led_to = std::string("lte_next"),
    };

    const json serialized = episode;

    EXPECT_EQ(serialized.at("outcome"), "resolved");
    EXPECT_EQ(serialized.at("created_at"), "2026-03-08T15:00:00Z");
    EXPECT_TRUE(serialized.at("caused_by").is_null());
    EXPECT_EQ(serialized.at("led_to"), "lte_next");

    const LongTermEpisode round_trip = serialized.get<LongTermEpisode>();
    EXPECT_EQ(round_trip.outcome, LongTermEpisodeOutcome::Resolved);
    EXPECT_EQ(round_trip.led_to, std::optional<std::string>("lte_next"));
    EXPECT_FALSE(round_trip.caused_by.has_value());
}

TEST(MemoryTypesTest, ConversationItemsRoundTripForEpisodeAndStubVariants) {
    const Conversation conversation{
        .items =
            {
                ConversationItem{
                    .type = ConversationItemType::OngoingEpisode,
                    .ongoing_episode =
                        OngoingEpisode{
                            .messages =
                                {
                                    Message{
                                        .role = MessageRole::User,
                                        .content = "hello",
                                        .create_time = json("2026-03-08T15:00:00Z").get<Timestamp>(),
                                    },
                                },
                        },
                    .episode_stub = std::nullopt,
                },
                ConversationItem{
                    .type = ConversationItemType::EpisodeStub,
                    .ongoing_episode = std::nullopt,
                    .episode_stub =
                        EpisodeStub{
                            .content = "[stub]",
                            .create_time = json("2026-03-08T15:01:00Z").get<Timestamp>(),
                        },
                },
            },
        .user_id = "user_001",
    };

    const json serialized = conversation;

    ASSERT_EQ(serialized.at("items").size(), 2);
    EXPECT_EQ(serialized.at("items")[0].at("type"), "ongoing_episode");
    EXPECT_EQ(serialized.at("items")[1].at("type"), "episode_stub");
    EXPECT_EQ(serialized.at("items")[0].at("ongoing_episode").at("messages")[0].at("content"),
              "hello");
    EXPECT_EQ(serialized.at("items")[1].at("episode_stub").at("content"), "[stub]");

    const Conversation round_trip = serialized.get<Conversation>();
    ASSERT_EQ(round_trip.items.size(), 2U);
    EXPECT_EQ(round_trip.items[0].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(round_trip.items[0].ongoing_episode.has_value());
    EXPECT_EQ(round_trip.items[0].ongoing_episode->messages[0].role, MessageRole::User);
    EXPECT_EQ(round_trip.items[1].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(round_trip.items[1].episode_stub.has_value());
    EXPECT_EQ(round_trip.items[1].episode_stub->content, "[stub]");
}

TEST(MemoryTypesTest, ConversationItemSerializationRejectsMissingTaggedPayload) {
    const ConversationItem invalid_episode_item{
        .type = ConversationItemType::OngoingEpisode,
        .ongoing_episode = std::nullopt,
        .episode_stub = std::nullopt,
    };
    const ConversationItem invalid_stub_item{
        .type = ConversationItemType::EpisodeStub,
        .ongoing_episode = std::nullopt,
        .episode_stub = std::nullopt,
    };

    EXPECT_THROW((void)json(invalid_episode_item), std::invalid_argument);
    EXPECT_THROW((void)json(invalid_stub_item), std::invalid_argument);
}

TEST(MemoryTypesTest, TimestampRoundTripsFractionalSeconds) {
    const Timestamp timestamp = json("2026-03-08T15:00:00.123Z").get<Timestamp>();

    EXPECT_EQ(json(timestamp), "2026-03-08T15:00:00.123Z");
}

TEST(MemoryTypesTest, TimestampParsesPositiveOffsetIntoUtc) {
    const Timestamp timestamp = json("2026-03-08T15:00:00+02:30").get<Timestamp>();

    EXPECT_EQ(json(timestamp), "2026-03-08T12:30:00Z");
}

TEST(MemoryTypesTest, TimestampParsesNegativeOffsetIntoUtc) {
    const Timestamp timestamp = json("2026-03-08T15:00:00-07:15").get<Timestamp>();

    EXPECT_EQ(json(timestamp), "2026-03-08T22:15:00Z");
}

TEST(MemoryTypesTest, TimestampRejectsInvalidStrings) {
    EXPECT_THROW((void)json("2026-03-08 15:00:00Z").get<Timestamp>(), std::invalid_argument);
    EXPECT_THROW((void)json("2026-13-08T15:00:00Z").get<Timestamp>(), std::invalid_argument);
    EXPECT_THROW((void)json("2026-03-08T15:00:00").get<Timestamp>(), std::invalid_argument);
    EXPECT_THROW((void)json("2026-03-08T15:00:00.1234Z").get<Timestamp>(), std::invalid_argument);
}

} // namespace
} // namespace isla::server::memory
