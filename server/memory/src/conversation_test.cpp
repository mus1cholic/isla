#include "isla/server/memory/conversation.hpp"

#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

void ExpectConversationJsonEq(const Conversation& conversation, const json& expected) {
    EXPECT_EQ(json(conversation).dump(2), expected.dump(2));
}

TEST(ConversationTest, AppendUserMessageCreatesFirstOngoingEpisode) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    AppendUserMessage(conversation, "hello", Ts("2026-03-08T14:00:00Z"));

    ExpectConversationJsonEq(conversation, json::parse(R"json(
{
  "items": [
    {
      "type": "ongoing_episode",
      "ongoing_episode": {
        "messages": [
          {
            "role": "user",
            "content": "hello",
            "create_time": "2026-03-08T14:00:00Z"
          }
        ]
      }
    }
  ],
  "user_id": "user_001"
}
)json"));
}

TEST(ConversationTest, BeginOngoingEpisodeStartsNewConversationItem) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    AppendUserMessage(conversation, "one", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "two", Ts("2026-03-08T14:00:02Z"));
    BeginOngoingEpisode(conversation);
    AppendAssistantMessage(conversation, "three", Ts("2026-03-08T14:00:03Z"));

    ASSERT_EQ(conversation.items.size(), 2U);
    ASSERT_TRUE(conversation.items[0].ongoing_episode.has_value());
    ASSERT_TRUE(conversation.items[1].ongoing_episode.has_value());
    EXPECT_EQ(conversation.items[0].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(conversation.items[1].ongoing_episode->messages.size(), 1U);

    ExpectConversationJsonEq(conversation, json::parse(R"json(
{
  "items": [
    {
      "type": "ongoing_episode",
      "ongoing_episode": {
        "messages": [
          {
            "role": "user",
            "content": "one",
            "create_time": "2026-03-08T14:00:01Z"
          },
          {
            "role": "assistant",
            "content": "two",
            "create_time": "2026-03-08T14:00:02Z"
          }
        ]
      }
    },
    {
      "type": "ongoing_episode",
      "ongoing_episode": {
        "messages": [
          {
            "role": "assistant",
            "content": "three",
            "create_time": "2026-03-08T14:00:03Z"
          }
        ]
      }
    }
  ],
  "user_id": "user_001"
}
)json"));
}

TEST(ConversationTest, AppendEpisodeStubAddsStandaloneStubItem) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    AppendUserMessage(conversation, "one", Ts("2026-03-08T14:00:01Z"));
    AppendEpisodeStub(conversation, "[stub]", Ts("2026-03-08T14:00:02Z"));

    ExpectConversationJsonEq(conversation, json::parse(R"json(
{
  "items": [
    {
      "type": "ongoing_episode",
      "ongoing_episode": {
        "messages": [
          {
            "role": "user",
            "content": "one",
            "create_time": "2026-03-08T14:00:01Z"
          }
        ]
      }
    },
    {
      "type": "episode_stub",
      "episode_stub": {
        "content": "[stub]",
        "create_time": "2026-03-08T14:00:02Z"
      }
    }
  ],
  "user_id": "user_001"
}
)json"));
}

TEST(ConversationTest, ReplaceOngoingEpisodeWithStubPreservesNeighbors) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    AppendUserMessage(conversation, "one", Ts("2026-03-08T14:00:01Z"));
    AppendEpisodeStub(conversation, "[existing stub]", Ts("2026-03-08T14:00:02Z"));
    BeginOngoingEpisode(conversation);
    AppendAssistantMessage(conversation, "two", Ts("2026-03-08T14:00:03Z"));

    ASSERT_TRUE(
        ReplaceOngoingEpisodeWithStub(conversation, 2, "[replaced]", Ts("2026-03-08T14:00:04Z"))
            .ok());

    ExpectConversationJsonEq(conversation, json::parse(R"json(
{
  "items": [
    {
      "type": "ongoing_episode",
      "ongoing_episode": {
        "messages": [
          {
            "role": "user",
            "content": "one",
            "create_time": "2026-03-08T14:00:01Z"
          }
        ]
      }
    },
    {
      "type": "episode_stub",
      "episode_stub": {
        "content": "[existing stub]",
        "create_time": "2026-03-08T14:00:02Z"
      }
    },
    {
      "type": "episode_stub",
      "episode_stub": {
        "content": "[replaced]",
        "create_time": "2026-03-08T14:00:04Z"
      }
    }
  ],
  "user_id": "user_001"
}
)json"));
}

TEST(ConversationTest, ReplaceOngoingEpisodeWithStubRejectsBadTargets) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    AppendUserMessage(conversation, "one", Ts("2026-03-08T14:00:01Z"));
    AppendEpisodeStub(conversation, "[existing stub]", Ts("2026-03-08T14:00:02Z"));

    EXPECT_FALSE(
        ReplaceOngoingEpisodeWithStub(conversation, 3, "[stub]", Ts("2026-03-08T14:00:03Z")).ok());
    EXPECT_FALSE(
        ReplaceOngoingEpisodeWithStub(conversation, 1, "[stub]", Ts("2026-03-08T14:00:03Z")).ok());
    EXPECT_FALSE(
        ReplaceOngoingEpisodeWithStub(conversation, 0, "", Ts("2026-03-08T14:00:03Z")).ok());
}

} // namespace
} // namespace isla::server::memory
