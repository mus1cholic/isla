#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/memory_orchestrator.hpp"

#include <string>

#include "absl/status/status.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;

class MemoryOrchestratorTest : public ::testing::Test {
  protected:
    static Timestamp Ts(std::string_view text) {
        return json(text).get<Timestamp>();
    }

    static MemoryOrchestrator MakeHandler() {
        return MemoryOrchestrator::Create("srv_test", WorkingMemoryInit{
                                                          .system_prompt = "You are Isla.",
                                                          .user_id = "user_001",
                                                      });
    }
};

TEST_F(MemoryOrchestratorTest, HandleUserQueryAppendsUserMessageAndRendersPrompt) {
    MemoryOrchestrator handler = MakeHandler();

    ASSERT_TRUE(handler
                    .HandleUserQuery(GatewayUserQuery{
                        .session_id = "srv_test",
                        .turn_id = "turn_001",
                        .text = "hello",
                        .create_time = Ts("2026-03-08T14:00:00Z"),
                    })
                    .ok());

    const absl::StatusOr<std::string> prompt = handler.RenderPrompt();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- [user | 2026-03-08T14:00:00Z] hello"), std::string::npos);

    const WorkingMemoryState& state = handler.memory().snapshot();
    ASSERT_EQ(state.conversation.items.size(), 1U);
    ASSERT_TRUE(state.conversation.items.front().ongoing_episode.has_value());
    const auto& messages = state.conversation.items.front().ongoing_episode->messages;
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].role, MessageRole::User);
    EXPECT_EQ(messages[0].content, "hello");
    EXPECT_FALSE(state.retrieved_memory.has_value());
}

TEST_F(MemoryOrchestratorTest, HandleUserQueryDoesNotDisturbExistingConversationItems) {
    MemoryOrchestrator handler = MakeHandler();
    AppendUserMessage(handler.mutable_memory().mutable_conversation(), "first",
                      Ts("2026-03-08T13:59:59Z"));
    handler.mutable_memory().AppendEpisodeStub("[previous topic]", Ts("2026-03-08T14:00:00Z"));

    ASSERT_TRUE(handler
                    .HandleUserQuery(GatewayUserQuery{
                        .session_id = "srv_test",
                        .turn_id = "turn_002",
                        .text = "second",
                        .create_time = Ts("2026-03-08T14:00:01Z"),
                    })
                    .ok());

    const WorkingMemoryState& state = handler.memory().snapshot();
    ASSERT_EQ(state.conversation.items.size(), 3U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
    EXPECT_EQ(state.conversation.items[1].type, ConversationItemType::EpisodeStub);
    EXPECT_EQ(state.conversation.items[2].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(state.conversation.items[2].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[2].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(state.conversation.items[2].ongoing_episode->messages[0].content, "second");
}

TEST_F(MemoryOrchestratorTest, HandleAssistantReplyAppendsAssistantMessage) {
    MemoryOrchestrator handler = MakeHandler();
    ASSERT_TRUE(handler
                    .HandleUserQuery(GatewayUserQuery{
                        .session_id = "srv_test",
                        .turn_id = "turn_001",
                        .text = "hello",
                        .create_time = Ts("2026-03-08T14:00:00Z"),
                    })
                    .ok());

    ASSERT_TRUE(handler
                    .HandleAssistantReply(GatewayAssistantReply{
                        .session_id = "srv_test",
                        .turn_id = "turn_001",
                        .text = "hi there",
                        .create_time = Ts("2026-03-08T14:00:01Z"),
                    })
                    .ok());

    const WorkingMemoryState& state = handler.memory().snapshot();
    ASSERT_EQ(state.conversation.items.size(), 1U);
    ASSERT_TRUE(state.conversation.items.front().ongoing_episode.has_value());
    const auto& messages = state.conversation.items.front().ongoing_episode->messages;
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].role, MessageRole::User);
    EXPECT_EQ(messages[1].role, MessageRole::Assistant);
    EXPECT_EQ(messages[1].content, "hi there");
}

TEST_F(MemoryOrchestratorTest, EndToEndConversationProducesExpectedWorkingMemoryAndPrompt) {
    MemoryOrchestrator orchestrator = MakeHandler();

    ASSERT_TRUE(orchestrator
                    .HandleUserQuery(GatewayUserQuery{
                        .session_id = "srv_test",
                        .turn_id = "turn_001",
                        .text = "Please help me plan Sarah's birthday.",
                        .create_time = Ts("2026-03-08T14:00:00Z"),
                    })
                    .ok());
    ASSERT_TRUE(orchestrator
                    .HandleAssistantReply(GatewayAssistantReply{
                        .session_id = "srv_test",
                        .turn_id = "turn_001",
                        .text = "I can help you plan it.",
                        .create_time = Ts("2026-03-08T14:00:05Z"),
                    })
                    .ok());

    const absl::StatusOr<std::string> prompt = orchestrator.RenderPrompt();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_EQ(*prompt, R"prompt({system_prompt}
You are Isla.
{persistent_memory_cache}
Active Models:
- (none)
Familiar Labels:
- (none)
{mid_term_episodes}
- (none)
{retrieved_memory}
(none)
{conversation}
- [user | 2026-03-08T14:00:00Z] Please help me plan Sarah's birthday.
- [assistant | 2026-03-08T14:00:05Z] I can help you plan it.
)prompt");
}

TEST_F(MemoryOrchestratorTest, ApplyCompletedEpisodeFlushDelegatesToWorkingMemory) {
    MemoryOrchestrator handler = MakeHandler();
    AppendUserMessage(handler.mutable_memory().mutable_conversation(), "one",
                      Ts("2026-03-08T14:00:00Z"));
    AppendAssistantMessage(handler.mutable_memory().mutable_conversation(), "two",
                           Ts("2026-03-08T14:00:01Z"));

    ASSERT_TRUE(handler
                    .ApplyCompletedEpisodeFlush(CompletedOngoingEpisodeFlush{
                        .conversation_item_index = 0,
                        .episode =
                            Episode{
                                .episode_id = "ep_001",
                                .tier1_detail = std::string("full detail"),
                                .tier2_summary = "summary",
                                .tier3_ref = "stub ref",
                                .tier3_keywords = { "memory" },
                                .salience = 8,
                                .embedding = {},
                                .created_at = Ts("2026-03-08T14:00:02Z"),
                            },
                        .stub_timestamp = Ts("2026-03-08T14:00:03Z"),
                    })
                    .ok());

    const WorkingMemoryState& state = handler.memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    EXPECT_EQ(state.mid_term_episodes[0].episode_id, "ep_001");
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[0].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[0].episode_stub->content, "stub ref");
}

TEST_F(MemoryOrchestratorTest, RejectsMismatchedSessionIds) {
    MemoryOrchestrator handler = MakeHandler();

    const absl::Status mismatched = handler.HandleUserQuery(GatewayUserQuery{
        .session_id = "srv_other",
        .turn_id = "turn_001",
        .text = "hello",
        .create_time = Ts("2026-03-08T14:00:00Z"),
    });

    ASSERT_FALSE(mismatched.ok());
    EXPECT_EQ(mismatched.code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(MemoryOrchestratorTest, RejectsMissingTurnId) {
    MemoryOrchestrator handler = MakeHandler();

    const absl::Status missing_turn = handler.HandleUserQuery(GatewayUserQuery{
        .session_id = "srv_test",
        .turn_id = "",
        .text = "hello",
        .create_time = Ts("2026-03-08T14:00:00Z"),
    });

    ASSERT_FALSE(missing_turn.ok());
    EXPECT_EQ(missing_turn.code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server::memory
