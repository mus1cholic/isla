#include "isla/server/memory/mid_term_flush_decider.hpp"

#include <gmock/gmock.h>
#include <memory>
#include <optional>
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

Conversation MakeSimpleConversation() {
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

Conversation MakeEightMessageConversation() {
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
            Message{.role = MessageRole::User, .content = "Topic B question",
                    .create_time = Ts("2026-03-14T10:02:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Topic B answer",
                    .create_time = Ts("2026-03-14T10:02:10Z")},
            Message{.role = MessageRole::User, .content = "Topic C question",
                    .create_time = Ts("2026-03-14T10:03:00Z")},
            Message{.role = MessageRole::Assistant, .content = "Topic C answer",
                    .create_time = Ts("2026-03-14T10:03:10Z")},
        },
    };
    conversation.items.push_back(std::move(item));
    return conversation;
}

TEST(LlmMidTermFlushDeciderTest, DecideReturnsNoFlushWhenLlmFindsNoBoundaries) {
    const std::string response =
        R"({"boundaries": [], "tail_complete": false, "reasoning": "Active discussion"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->boundaries.empty());
    EXPECT_FALSE(decision->tail_complete);
    ASSERT_TRUE(decision->reasoning.has_value());
    EXPECT_EQ(*decision->reasoning, "Active discussion");
}

TEST(LlmMidTermFlushDeciderTest, DecideReturnsMultipleBoundaries) {
    const std::string response = R"json({
        "boundaries": [
            { "starts_at": "m4", "reasoning": "Topic B begins here." },
            { "starts_at": "m6", "reasoning": "Topic C begins here." }
        ],
        "tail_complete": true,
        "reasoning": "Three completed topics were found."
    })json";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeEightMessageConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    ASSERT_EQ(decision->boundaries.size(), 2U);
    EXPECT_EQ(decision->boundaries[0].starts_at_message_index, 4U);
    EXPECT_EQ(decision->boundaries[1].starts_at_message_index, 6U);
    EXPECT_TRUE(decision->tail_complete);
}

TEST(LlmMidTermFlushDeciderTest, DecideReturnsTailCompleteWithoutBoundaries) {
    const std::string response =
        R"({"boundaries": [], "tail_complete": true, "reasoning": "Topic concluded"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->boundaries.empty());
    EXPECT_TRUE(decision->tail_complete);
}

TEST(LlmMidTermFlushDeciderTest, DecidePassesCorrectJsonToLlm) {
    const std::string response =
        R"({"boundaries": [], "tail_complete": false, "reasoning": "No boundary"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeConversationWithStubAndEpisode();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    const json sent = json::parse(last_request->user_text);
    ASSERT_TRUE(sent.contains("items"));
    ASSERT_EQ(sent["items"].size(), 2U);
    EXPECT_EQ(sent["items"][0]["id"], "i0");
    EXPECT_EQ(sent["items"][0]["type"], "episode_stub");
    EXPECT_EQ(sent["items"][1]["id"], "i1");
    EXPECT_EQ(sent["items"][1]["type"], "ongoing_episode");
    EXPECT_EQ(sent["items"][1]["ongoing_episode"]["messages"][0]["id"], "m0");
    EXPECT_EQ(sent["items"][1]["ongoing_episode"]["messages"][0]["role"], "user");
}

TEST(LlmMidTermFlushDeciderTest, DecideUsesCorrectModelAndSystemPrompt) {
    const std::string response = R"({"boundaries": [], "tail_complete": false, "reasoning": "No"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    EXPECT_EQ(last_request->model, "test-model");
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

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingBoundariesField) {
    auto [fake, last_request, decider] = MakeDecider(R"({"tail_complete": false})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingTailCompleteField) {
    auto [fake, last_request, decider] = MakeDecider(R"({"boundaries": []})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidBoundaryIdFormat) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"boundaries": [{"starts_at": "bad"}], "tail_complete": false, "reasoning": "No"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsAssistantBoundary) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"boundaries": [{"starts_at": "m3"}], "tail_complete": false, "reasoning": "No"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsBoundaryThatLeavesTooFewMessagesBeforeIt) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"boundaries": [{"starts_at": "m0"}], "tail_complete": false, "reasoning": "No"})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsBoundaryThatLeavesTooShortTail) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"boundaries": [{"starts_at": "m2"}, {"starts_at": "m3"}], "tail_complete": false})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsUnorderedBoundaries) {
    auto [fake, last_request, decider] = MakeDecider(
        R"({"boundaries": [{"starts_at": "m4"}, {"starts_at": "m2"}], "tail_complete": false})");
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeEightMessageConversation();
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

TEST(LlmMidTermFlushDeciderTest, DecideHandlesEmptyConversationWhenNoFlushRequested) {
    const std::string response =
        R"({"boundaries": [], "tail_complete": false, "reasoning": "Empty"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    Conversation conversation;
    conversation.user_id = "user_test";
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->boundaries.empty());
    EXPECT_FALSE(decision->tail_complete);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsBoundariesForEmptyConversation) {
    const std::string response =
        R"({"boundaries": [{"starts_at": "m0"}], "tail_complete": false, "reasoning": "Empty"})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    Conversation conversation;
    conversation.user_id = "user_test";
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
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
                                 R"({"boundaries": [], "tail_complete": false})"
                                 "\n```";
    const DeciderWithFake built = MakeDecider(response);
    ASSERT_NE(built.decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = built.decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_TRUE(decision->boundaries.empty());
    EXPECT_FALSE(decision->tail_complete);
}

TEST(LlmMidTermFlushDeciderTest, DecideExtractsBoundaryAndTopLevelReasoning) {
    const std::string response = R"json({
        "boundaries": [
            { "starts_at": "m2", "reasoning": "A new topic starts here." }
        ],
        "tail_complete": false,
        "reasoning": "One boundary detected."
    })json";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    ASSERT_EQ(decision->boundaries.size(), 1U);
    ASSERT_TRUE(decision->boundaries[0].reasoning.has_value());
    EXPECT_EQ(*decision->boundaries[0].reasoning, "A new topic starts here.");
    ASSERT_TRUE(decision->reasoning.has_value());
    EXPECT_EQ(*decision->reasoning, "One boundary detected.");
}

TEST(LlmMidTermFlushDeciderTest, DecideReasoningIsNulloptWhenAbsentOrNull) {
    auto [fake1, last_request1, decider1] =
        MakeDecider(R"({"boundaries": [], "tail_complete": false})");
    ASSERT_NE(decider1, nullptr);
    auto [fake2, last_request2, decider2] =
        MakeDecider(R"({"boundaries": [], "tail_complete": false, "reasoning": null})");
    ASSERT_NE(decider2, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision1 = decider1->Decide(conversation);
    const absl::StatusOr<MidTermFlushDecision> decision2 = decider2->Decide(conversation);

    ASSERT_TRUE(decision1.ok()) << decision1.status();
    ASSERT_TRUE(decision2.ok()) << decision2.status();
    EXPECT_FALSE(decision1->reasoning.has_value());
    EXPECT_FALSE(decision2->reasoning.has_value());
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsReasoningWithWrongType) {
    const std::string response = R"({"boundaries": [], "tail_complete": false, "reasoning": 42})";
    auto [fake, last_request, decider] = MakeDecider(response);
    ASSERT_NE(decider, nullptr);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
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
        "boundaries": [],
        "tail_complete": false,
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
        "boundaries": [],
        "tail_complete": false,
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
