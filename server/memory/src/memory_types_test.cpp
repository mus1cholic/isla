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
  "system_prompt": "You are Isla.",
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
    "messages": [
      {
        "role": "user",
        "content": "Please fix the renderer.",
        "create_time": "2026-03-08T14:00:00Z"
      },
      {
        "role": "stub",
        "content": "[debugging inverted normals - resolved]",
        "create_time": "2026-03-08T14:04:00Z"
      }
    ],
    "user_id": "user_001",
    "conversation_id": "conversation_001",
    "session_id": "session_001"
  },
  "created_at": "2026-03-08T14:00:00Z",
  "ended_at": null
}
)json");

    const Session session = input.get<Session>();

    EXPECT_EQ(session.session_id, "session_001");
    ASSERT_EQ(session.persistent_memory_cache.active_models.size(), 1);
    EXPECT_EQ(session.persistent_memory_cache.active_models.front().entity_id, "entity_sarah");
    ASSERT_EQ(session.mid_term_episodes.size(), 1);
    EXPECT_EQ(session.mid_term_episodes.front().salience, 9);
    ASSERT_TRUE(session.retrieved_memory.has_value());
    EXPECT_EQ(*session.retrieved_memory, "placeholder retrieved memory");
    ASSERT_EQ(session.conversation.messages.size(), 2);
    EXPECT_EQ(session.conversation.messages[1].role, MessageRole::Stub);
    EXPECT_EQ(json(session.created_at), "2026-03-08T14:00:00Z");
    EXPECT_EQ(json(session.mid_term_episodes.front().created_at), "2026-03-08T14:05:00Z");
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

} // namespace
} // namespace isla::server::memory
