#include "isla/server/memory/mid_term_flush_decider.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

// ---------------------------------------------------------------------------
// Fake OpenAI client that delivers a canned response
// ---------------------------------------------------------------------------

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit FakeOpenAiResponsesClient(std::string canned_response)
        : canned_response_(std::move(canned_response)) {}

    FakeOpenAiResponsesClient(std::string canned_response, absl::Status stream_status)
        : canned_response_(std::move(canned_response)), stream_status_(std::move(stream_status)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        last_request_model_ = request.model;
        last_request_system_prompt_ = request.system_prompt;
        last_request_user_text_ = request.user_text;

        if (!stream_status_.ok()) {
            return stream_status_;
        }

        // Deliver canned response as a single text delta + completed event.
        if (!canned_response_.empty()) {
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = canned_response_ });
            if (!delta_status.ok()) {
                return delta_status;
            }
        }
        absl::Status completed_status =
            on_event(OpenAiResponsesCompletedEvent{ .response_id = "resp_test" });
        if (!completed_status.ok()) {
            return completed_status;
        }
        return absl::OkStatus();
    }

    [[nodiscard]] const std::string& last_request_model() const {
        return last_request_model_;
    }
    [[nodiscard]] const std::string& last_request_system_prompt() const {
        return last_request_system_prompt_;
    }
    [[nodiscard]] const std::string& last_request_user_text() const {
        return last_request_user_text_;
    }

  private:
    std::string canned_response_;
    absl::Status stream_status_ = absl::OkStatus();
    mutable std::string last_request_model_;
    mutable std::string last_request_system_prompt_;
    mutable std::string last_request_user_text_;
};

// ---------------------------------------------------------------------------
// Helper: build a decider with a canned LLM response
// ---------------------------------------------------------------------------

struct DeciderWithFake {
    std::shared_ptr<FakeOpenAiResponsesClient> fake_client;
    MidTermFlushDeciderPtr decider;
};

// NOTE: These helpers CHECK that the factory succeeds. If the embedded system
// prompt fails to load, CHECK will abort with a clear message rather than
// letting tests segfault on a null decider.

DeciderWithFake MakeDecider(std::string canned_response) {
    auto fake = std::make_shared<FakeOpenAiResponsesClient>(std::move(canned_response));
    MidTermFlushDeciderPtr decider = CreateLlmMidTermFlushDecider(fake, "test-model");
    CHECK(decider != nullptr) << "Factory returned null — embedded system prompt may be missing";
    return { fake, std::move(decider) };
}

DeciderWithFake MakeFailingDecider(absl::Status failure_status) {
    auto fake = std::make_shared<FakeOpenAiResponsesClient>("", std::move(failure_status));
    MidTermFlushDeciderPtr decider = CreateLlmMidTermFlushDecider(fake, "test-model");
    CHECK(decider != nullptr) << "Factory returned null — embedded system prompt may be missing";
    return { fake, std::move(decider) };
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
    auto [fake, decider] = MakeDecider(response);

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
    auto [fake, decider] = MakeDecider(response);

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
    auto [fake, decider] = MakeDecider(response);

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
    auto [fake, decider] = MakeDecider(response);

    const Conversation conversation = MakeConversationWithStubAndEpisode();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    // Parse the user_text that was sent to the LLM.
    const json sent = json::parse(fake->last_request_user_text());

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
    auto [fake, decider] = MakeDecider(response);

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);
    ASSERT_TRUE(decision.ok()) << decision.status();

    EXPECT_EQ(fake->last_request_model(), "test-model");
    // Verify the system prompt matches the embedded asset (correct wiring).
    const absl::StatusOr<std::string> expected_prompt =
        LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    ASSERT_TRUE(expected_prompt.ok()) << expected_prompt.status();
    EXPECT_EQ(fake->last_request_system_prompt(), *expected_prompt);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidJson) {
    auto [fake, decider] = MakeDecider("not valid json at all");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingShouldFlushField) {
    auto [fake, decider] = MakeDecider(R"({"item_id": "i0"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsMissingItemIdWhenFlushing) {
    auto [fake, decider] =
        MakeDecider(R"({"should_flush": true, "split_at": null, "reasoning": "Yes"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidItemIdFormat) {
    auto [fake, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "bad", "split_at": null, "reasoning": "Yes"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInvalidSplitAtFormat) {
    auto [fake, decider] = MakeDecider(
        R"({"should_flush": true, "item_id": "i0", "split_at": "bad", "reasoning": "Yes"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecidePropagatesLlmFailure) {
    auto [fake, decider] = MakeFailingDecider(absl::InternalError("LLM service unavailable"));

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInternal);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInconsistentNoFlushWithItemId) {
    auto [fake, decider] = MakeDecider(
        R"({"should_flush": false, "item_id": "i0", "split_at": null, "reasoning": "No"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsInconsistentNoFlushWithSplitAt) {
    auto [fake, decider] = MakeDecider(
        R"({"should_flush": false, "item_id": null, "split_at": "m3", "reasoning": "No"})");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, DecideHandlesEmptyConversation) {
    const std::string response =
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "Empty"})";
    auto [fake, decider] = MakeDecider(response);

    Conversation conversation;
    conversation.user_id = "user_test";
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_TRUE(decision.ok()) << decision.status();
    EXPECT_FALSE(decision->should_flush);

    // Verify the serialized JSON has an empty items array.
    const json sent = json::parse(fake->last_request_user_text());
    EXPECT_TRUE(sent["items"].empty());
}

TEST(LlmMidTermFlushDeciderTest, DecideRejectsEmptyLlmResponse) {
    auto fake = std::make_shared<FakeOpenAiResponsesClient>("");
    MidTermFlushDeciderPtr decider = CreateLlmMidTermFlushDecider(fake, "test-model");

    const Conversation conversation = MakeSimpleConversation();
    const absl::StatusOr<MidTermFlushDecision> decision = decider->Decide(conversation);

    ASSERT_FALSE(decision.ok());
    EXPECT_EQ(decision.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermFlushDeciderTest, FactoryReturnsNonNullDecider) {
    auto fake = std::make_shared<FakeOpenAiResponsesClient>(
        R"({"should_flush": false, "item_id": null, "split_at": null, "reasoning": "test"})");
    MidTermFlushDeciderPtr decider = CreateLlmMidTermFlushDecider(fake, "gpt-4o-mini");

    ASSERT_NE(decider, nullptr);
}

TEST(LlmMidTermFlushDeciderTest, FactoryReturnsNullForNullClient) {
    MidTermFlushDeciderPtr decider = CreateLlmMidTermFlushDecider(nullptr, "test-model");

    EXPECT_EQ(decider, nullptr);
}

} // namespace
} // namespace isla::server::memory
