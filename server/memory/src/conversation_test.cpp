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

TEST(ConversationTest, SplitOngoingEpisodeWithStubSplitsCorrectly) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    // Build a 6-message ongoing episode: U A U A U A
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(conversation, "a2", Ts("2026-03-08T14:00:04Z"));
    AppendUserMessage(conversation, "u3", Ts("2026-03-08T14:00:05Z"));
    AppendAssistantMessage(conversation, "a3", Ts("2026-03-08T14:00:06Z"));

    // Split at index 4 (a user message): completed = [u1, a1, u2, a2], remaining = [u3, a3]
    ASSERT_TRUE(
        SplitOngoingEpisodeWithStub(conversation, 0, 4, "[split stub]", Ts("2026-03-08T14:00:07Z"))
            .ok());

    ASSERT_EQ(conversation.items.size(), 2U);

    // Item 0 should be the stub.
    EXPECT_EQ(conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(conversation.items[0].episode_stub.has_value());
    EXPECT_EQ(conversation.items[0].episode_stub->content, "[split stub]");
    EXPECT_EQ(conversation.items[0].episode_stub->create_time, Ts("2026-03-08T14:00:07Z"));
    EXPECT_FALSE(conversation.items[0].ongoing_episode.has_value());

    // Item 1 should be the remaining ongoing episode with 2 messages.
    EXPECT_EQ(conversation.items[1].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(conversation.items[1].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(conversation.items[1].ongoing_episode->messages[0].content, "u3");
    EXPECT_EQ(conversation.items[1].ongoing_episode->messages[0].role, MessageRole::User);
    EXPECT_EQ(conversation.items[1].ongoing_episode->messages[1].content, "a3");
    EXPECT_EQ(conversation.items[1].ongoing_episode->messages[1].role, MessageRole::Assistant);
    EXPECT_FALSE(conversation.items[1].episode_stub.has_value());
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubPreservesNeighbors) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };

    // Item 0: episode stub
    AppendEpisodeStub(conversation, "[earlier stub]", Ts("2026-03-08T13:59:00Z"));
    // Item 1: ongoing episode with 4 messages
    BeginOngoingEpisode(conversation);
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(conversation, "a2", Ts("2026-03-08T14:00:04Z"));

    // Split at index 2 in item 1: completed = [u1, a1], remaining = [u2, a2]
    ASSERT_TRUE(
        SplitOngoingEpisodeWithStub(conversation, 1, 2, "[split]", Ts("2026-03-08T14:00:05Z"))
            .ok());

    ASSERT_EQ(conversation.items.size(), 3U);
    EXPECT_EQ(conversation.items[0].type, ConversationItemType::EpisodeStub);
    EXPECT_EQ(conversation.items[0].episode_stub->content, "[earlier stub]");
    EXPECT_EQ(conversation.items[1].type, ConversationItemType::EpisodeStub);
    EXPECT_EQ(conversation.items[1].episode_stub->content, "[split]");
    EXPECT_EQ(conversation.items[2].type, ConversationItemType::OngoingEpisode);
    ASSERT_EQ(conversation.items[2].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(conversation.items[2].ongoing_episode->messages[0].content, "u2");
    EXPECT_EQ(conversation.items[2].ongoing_episode->messages[1].content, "a2");
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubRejectsOutOfRange) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(conversation, "a2", Ts("2026-03-08T14:00:04Z"));

    // conversation_item_index out of range
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 5, 2, "[stub]", Ts("2026-03-08T14:00:05Z")).ok());

    // split_at_message_index out of range (>= message count)
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 10, "[stub]", Ts("2026-03-08T14:00:05Z"))
            .ok());
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubRejectsTooFewMessages) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(conversation, "a2", Ts("2026-03-08T14:00:04Z"));

    // split_at < 2
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 1, "[stub]", Ts("2026-03-08T14:00:05Z")).ok());
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 0, "[stub]", Ts("2026-03-08T14:00:05Z")).ok());
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubRejectsAssistantAtSplitPoint) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));
    AppendAssistantMessage(conversation, "a2", Ts("2026-03-08T14:00:04Z"));

    // Index 3 is an assistant message
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 3, "[stub]", Ts("2026-03-08T14:00:05Z")).ok());
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubRejectsNonOngoingEpisode) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };
    AppendEpisodeStub(conversation, "[a stub]", Ts("2026-03-08T14:00:01Z"));

    // Index 0 is an episode stub, not an ongoing episode
    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 2, "[stub]", Ts("2026-03-08T14:00:05Z")).ok());
}

TEST(ConversationTest, SplitOngoingEpisodeWithStubRejectsEmptyStubText) {
    Conversation conversation{
        .items = {},
        .user_id = "user_001",
    };
    AppendUserMessage(conversation, "u1", Ts("2026-03-08T14:00:01Z"));
    AppendAssistantMessage(conversation, "a1", Ts("2026-03-08T14:00:02Z"));
    AppendUserMessage(conversation, "u2", Ts("2026-03-08T14:00:03Z"));

    EXPECT_FALSE(
        SplitOngoingEpisodeWithStub(conversation, 0, 2, "", Ts("2026-03-08T14:00:05Z")).ok());
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
