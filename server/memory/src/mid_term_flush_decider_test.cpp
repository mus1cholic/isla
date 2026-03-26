#include "isla/server/memory/mid_term_flush_decider.hpp"

#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "server/src/llm_client_mock.hpp"

namespace isla::server::memory {
namespace {

using isla::server::LlmRequest;
using nlohmann::json;
using ::testing::_;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

struct DeciderWithFake {
    std::shared_ptr<isla::server::test::MockLlmClient> fake_client;
    std::shared_ptr<LlmRequest> last_request;
    MidTermFlushDeciderPtr decider;
};

DeciderWithFake
MakeDecider(std::string canned_response,
            std::optional<isla::server::LlmReasoningEffort> reasoning_effort = std::nullopt) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto last_request = std::make_shared<LlmRequest>();
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([last_request, canned_response = std::move(canned_response)](
                      const LlmRequest& request, const isla::server::LlmEventCallback& on_event) {
            *last_request = request;
            return isla::server::test::EmitLlmResponse(canned_response, on_event);
        });
    absl::StatusOr<MidTermFlushDeciderPtr> decider =
        reasoning_effort.has_value()
            ? CreateLlmMidTermFlushDecider(fake, "test-model", *reasoning_effort)
            : CreateLlmMidTermFlushDecider(fake, "test-model");
    if (!decider.ok()) {
        return { .fake_client = fake, .last_request = last_request, .decider = nullptr };
    }
    return { .fake_client = fake, .last_request = last_request, .decider = std::move(*decider) };
}

DeciderWithFake MakeFailingDecider(absl::Status failure_status) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto last_request = std::make_shared<LlmRequest>();
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([last_request, failure_status = std::move(failure_status)](
                      const LlmRequest& request,
                      const isla::server::LlmEventCallback& on_event) -> absl::Status {
            static_cast<void>(on_event);
            *last_request = request;
            return failure_status;
        });
    absl::StatusOr<MidTermFlushDeciderPtr> decider =
        CreateLlmMidTermFlushDecider(fake, "test-model");
    if (!decider.ok()) {
        return { .fake_client = fake, .last_request = last_request, .decider = nullptr };
    }
    return { .fake_client = fake, .last_request = last_request, .decider = std::move(*decider) };
}

// ---------------------------------------------------------------------------
// Helper: build test conversations
// ---------------------------------------------------------------------------

Conversation MakeSimpleConversation() {
    // 1 ongoing episode with 4 messages (2 user + 2 assistant turns).
    Conversation conversation;
    conversation.user_id = "user_test";
    ConversationItem item;
    item.type = ConversationItemType::OngoingEpisode;
    item.ongoing_episode = OngoingEpisode{
        .messages = {
            Message{.role = MessageRole::User, .content = "Hello",
                    .create_time = Ts("2026-03-14T10:00:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Hi there!",
                    .create_time = Ts("2026-03-14T10:00:10Z")},
            Message{.role = MessageRole::User, .content = "How are you?",
                    .create_time = Ts("2026-03-14T10:01:00Z")},
            Message{.role = MessageRole::Assistant, .content = "I'm doing well!",
                    .create_time = Ts("2026-03-14T10:01:10Z")},
        },
    };
    conversation.items.push_back(std::move(item));
    return conversation;
}

Conversation MakeConversationWithStubAndEpisode() {
    // 1 episode stub + 1 ongoing episode with 4 messages.
    Conversation conversation;
    conversation.user_id = "user_test";

    ConversationItem stub;
    stub.type = ConversationItemType::EpisodeStub;
    stub.episode_stub = EpisodeStub{
        .content = "Discussed weekend plans.",
        .create_time = Ts("2026-03-14T09:00:00Z"),
    };
    conversation.items.push_back(std::move(stub));

    ConversationItem oe;
    oe.type = ConversationItemType::OngoingEpisode;
    oe.ongoing_episode = OngoingEpisode{
        .messages = {
            Message{.role = MessageRole::User, .content = "What about dinner?",
                    .create_time = Ts("2026-03-14T10:00:00Z")},
            Message{.role = MessageRole::Assistant, .content = "How about Italian?",
                    .create_time = Ts("2026-03-14T10:00:10Z")},
            Message{.role = MessageRole::User, .content = "Sounds good, thanks!",
                    .create_time = Ts("2026-03-14T10:01:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Great choice!",
                    .create_time = Ts("2026-03-14T10:01:10Z")},
        },
    };
    conversation.items.push_back(std::move(oe));
    return conversation;
}

Conversation MakeSixMessageConversation() {
    // 1 ongoing episode with 6 messages for split testing.
    Conversation conversation;
    conversation.user_id = "user_test";
    ConversationItem item;
    item.type = ConversationItemType::OngoingEpisode;
    item.ongoing_episode = OngoingEpisode{
        .messages = {
            Message{.role = MessageRole::User, .content = "Topic A question",
                    .create_time = Ts("2026-03-14T10:00:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Topic A answer",
                    .create_time = Ts("2026-03-14T10:00:10Z")},
            Message{.role = MessageRole::User, .content = "Topic A followup",
                    .create_time = Ts("2026-03-14T10:01:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Topic A detail",
                    .create_time = Ts("2026-03-14T10:01:10Z")},
            Message{.role = MessageRole::User, .content = "New topic B",
                    .create_time = Ts("2026-03-14T10:02:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Topic B answer",
                    .create_time = Ts("2026-03-14T10:02:10Z")},
        },
    };
    conversation.items.push_back(std::move(item));
    return conversation;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(LlmMidTermFlushDeciderTest, DecideReturnsNoFlushWhenLlmSaysNo) {
    const std::string response =
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "Active discussion"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_FALSE(decision->should_flush);
    EXPECT_FALSE(decision->conversation_item_index.has_value());
    EXPECT_FALSE(decision->split_at_message_index.has_value());
}

TEST(LlmMidTermFlushDeciderTest, DecideReturnsFullFlushWhenLlmFlushesEntireEpisode) {
    const std::string response =
        R"({"should_flush": true, "item_id": "i1", "split_at": null, "reasoning": "Topic completed"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeConversationWithStubAndEpisode();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->should_flush);
    ASSERT_TRUE(decision->conversation_item_index.has_value());
    EXPECT_EQ(*decision->conversation_item_index, 1U);
    EXPECT_FALSE(decision->split_at_message_index.has_value());
}

TEST(LlmMidTermFlushDeciderTest, DecideReturnsSplitFlushWhenLlmSplits) {
    const std::string response =
        R"({"should_flush": true, "item_id": "i0", "split_at": "m4", "reasoning": "Topic shift at m4"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSixMessageConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->should_flush);
    ASSERT_TRUE(decision->conversation_item_index.has_value());
    EXPECT_EQ(*decision->conversation_item_index, 0U);
    ASSERT_TRUE(decision->split_at_message_index.has_value());
    EXPECT_EQ(*decision->split_at_message_index, 4U);
}

TEST(LlmMidTermFlushDeciderTest, DecidePassesCorrectJsonToLlm) {
    const std::string response =
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "No boundary"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeConversationWithStubAndEpisode();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    // Parse the user_text that was sent to the LLM.
    const json sent = json::parse(last_request->user_text);

    ASSERT_TRUE(sent.contains("items"));
    ASSERT_EQ(sent["items"].size(), 2U);

    // Item 0: episode stub.
    const json& stub_item = sent["items"][0];
    EXPECT_EQ(stub_item["id"], "i0");
    EXPECT_EQ(stub_item["type"], "episode_stub");
    EXPECT_EQ(stub_item["episode_stub"]["content"], "Discussed weekend plans.");

    // Item 1: ongoing episode.
    const json& oe_item = sent["items"][1];
    EXPECT_EQ(oe_item["id"], "i1");
    EXPECT_EQ(oe_item["type"], "ongoing_episode");
    ASSERT_EQ(oe_item["ongoing_episode"]["messages"].size(), 4U);
    EXPECT_EQ(oe_item["ongoing_episode"]["messages"][0]["id"], "m0");
    EXPECT_EQ(oe_item["ongoing_episode"]["messages"][0]["role"], "user");
    EXPECT_EQ(oe_item["ongoing_episode"]["messages"][0]["content"], "What about dinner?");
    EXPECT_EQ(oe_item["ongoing_episode"]["messages"][1]["id"], "m1");
    EXPECT_EQ(oe_item["ongoing_episode"]["messages"][1]["role"], "assistant");
}

TEST(LlmMidTermFlushDeciderTest, DecideUsesCorrectModelAndSystemPrompt) {
    const std::string response =
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "No"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    EXPECT_EQ(last_request->model, "test-model");
    // Verify the system prompt matches the embedded asset (correct wiring).
    const absl::StatusOr<std::string> expected_prompt =
        LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    ASSERT_TRUE(expected_prompt.ok()) << expected_prompt.status();
    EXPECT_EQ(last_request->system_prompt, *expected_prompt);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidJson) {
    auto [fake, last_request, decider] = MakeDecider("not valid json at all");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingShouldFlushField) {
    auto [fake, last_request, decider] = MakeDecider(R"({"item_id": "i0"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingItemIdWhenFlushing) {
    auto [fake, last_request, decider] =
        MakeDecider(R"({"should_flush": true, "split_at": null, "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidItemIdFormat) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "bad", "split_at": null, "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidSplitAtFormat) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "i0", "split_at": "bad", "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecidePropagatesLlmFailure) {
    auto [fake, last_request, decider] =
        MakeFailingDecider(absl::InternalError("LLM service unavailable"));
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInternal);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInconsistentNoFlushWithItemId) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": false, "item_id": "i0", "split_at": null, "reasoning": "No"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInconsistentNoFlushWithSplitAt) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": false, "item_id": null, "split_at": "m3", "reasoning": "No"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsItemIndexOutOfBounds) {
    // Conversation has 1 item (index 0), but LLM returns item_id "i5".
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "i5", "split_at": null, "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsSplitAtOutOfBounds) {
    // Conversation has 4 messages (indices 0-3), but LLM returns split_at "m10".
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "i0", "split_at": "m10", "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsSplitAtOnEpisodeStub) {
    // LLM tries to split an episode stub (item i0), which has no messages.
    auto [fake, last_request, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "i0", "split_at": "m0", "reasoning": "Yes"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeConversationWithStubAndEpisode();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideHandlesEmptyConversation) {
    const std::string response =
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "Empty"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    Conversation conversation;
    conversation.user_id = "user_test";
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_FALSE(decision->should_flush);

    // Verify the serialized JSON has an empty items array.
    const json sent = json::parse(last_request->user_text);
    EXPECT_TRUE(sent["items"].empty());
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsEmptyLlmResponse) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([](const LlmRequest& request, const isla::server::LlmEventCallback& on_event) {
            static_cast<void>(request);
            return isla::server::test::EmitLlmResponse("", on_event);
        });
    absl::StatusOr<MidTermFlushDeciderPtr> decider =
        CreateLlmMidTermFlushDecider(fake, "test-model");
    ASSERT_TRUE(decider.ok()) << decider.status();
    ASSERT_NE(*decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = (*decider)->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideStripsMarkdownCodeFencesAndRetries) {
    const std::string response = "```json\n"
                                 R"({"should_flush": false})"
                                 "\n```";
    const DeciderWithFake built = MakeDecider(response);
    ASSERT_NE(built.decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = built.decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_FALSE(decision->should_flush);
}

TEST(LlmMidTermFlushDeciderTest, FactorySucceedsWithValidInputs) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    absl::StatusOr<MidTermFlushDeciderPtr> decider =
        CreateLlmMidTermFlushDecider(fake, "gpt-4o-mini");

    ASSERT_TRUE(decider.ok()) << decider.status();
    EXPECT_NE(*decider, nullptr);
}

TEST(LlmMidTermFlushDeciderTest, FactoryFailsForNullClient) {
    absl::StatusOr<MidTermFlushDeciderPtr> decider =
        CreateLlmMidTermFlushDecider(nullptr, "test-model");

    ASSERT_FALSE(decider.ok());
    EXPECT_EQ(decider.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, FactoryFailsForEmptyModel) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    absl::StatusOr<MidTermFlushDeciderPtr> decider = CreateLlmMidTermFlushDecider(fake, "");

    ASSERT_FALSE(decider.ok());
    EXPECT_EQ(decider.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DefaultReasoningEffortIsNone) {
    auto [fake_client, last_request, decider] = MakeDecider(R"json({
        "should_flush": false,
        "item_id": null,
        "split_at": null,
        "reasoning": "Nothing to flush."
    })json");
    ASSERT_NE(decider, nullptr);

    Conversation conversation = MakeSimpleConversation();
    const auto result = decider->Decide(conversation);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(last_request->reasoning_effort, isla::server::LlmReasoningEffort::kNone);
}

TEST(LlmMidTermFlushDeciderTest, ReasoningEffortIsConfigurable) {
    auto [fake_client, last_request, decider] =
        MakeDecider(R"json({
        "should_flush": false,
        "item_id": null,
        "split_at": null,
        "reasoning": "Nothing to flush."
    })json",
                    isla::server::LlmReasoningEffort::kHigh);
    ASSERT_NE(decider, nullptr);

    Conversation conversation = MakeSimpleConversation();
    const auto result = decider->Decide(conversation);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(last_request->reasoning_effort, isla::server::LlmReasoningEffort::kHigh);
}

} // namespace
} // namespace isla::server::memory
