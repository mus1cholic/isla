#include "isla/server/evals/eval_runner.hpp"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/statusor.h"
#include <gtest/gtest.h>

#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::ConversationMessageWrite;
using isla::server::memory::Episode;
using isla::server::memory::MemorySessionRecord;
using isla::server::memory::MemoryStore;
using isla::server::memory::MemoryStoreSnapshot;
using isla::server::memory::MessageRole;
using isla::server::memory::ParseTimestamp;
using isla::server::memory::Timestamp;
using isla::server::memory::UserWorkingMemoryRecord;
using namespace std::chrono_literals;

Timestamp Ts(std::string_view text) {
    return ParseTimestamp(text);
}

class RecordingMemoryStore final : public MemoryStore {
  public:
    absl::Status UpsertSession(const MemorySessionRecord& record) override {
        session_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status UpsertUserWorkingMemory(const UserWorkingMemoryRecord& record) override {
        user_working_memory_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(const ConversationMessageWrite& write) override {
        message_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& write) override {
        stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& write) override {
        mid_term_episode_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& write) override {
        split_stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ClearSessionWorkingSet(std::string_view session_id) override {
        static_cast<void>(session_id);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::vector<Episode>{};
    }

    absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const override {
        static_cast<void>(session_id);
        static_cast<void>(episode_id);
        return std::nullopt;
    }

    absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::nullopt;
    }

    std::vector<MemorySessionRecord> session_records;
    std::vector<UserWorkingMemoryRecord> user_working_memory_records;
    std::vector<ConversationMessageWrite> message_writes;
    std::vector<isla::server::memory::EpisodeStubWrite> stub_writes;
    std::vector<isla::server::memory::SplitEpisodeStubWrite> split_stub_writes;
    std::vector<isla::server::memory::MidTermEpisodeWrite> mid_term_episode_writes;
};

absl::Status EmitResponseText(std::string_view text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id = "resp_test") {
    absl::Status delta_status = on_event(
        isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent{ .text_delta = std::string(text) });
    if (!delta_status.ok()) {
        return delta_status;
    }
    return on_event(isla::server::ai_gateway::OpenAiResponsesCompletedEvent{
        .response_id = std::string(response_id),
    });
}

absl::Status EmitMidTermAwareReply(std::string_view decider_prompt,
                                   std::string_view compactor_prompt,
                                   const OpenAiResponsesRequest& request,
                                   const OpenAiResponsesEventCallback& on_event,
                                   std::string_view main_prefix = "stub reply: ") {
    if (request.system_prompt == decider_prompt) {
        return EmitResponseText(R"json({
            "should_flush": false,
            "item_id": null,
            "split_at": null,
            "reasoning": "No completed episode boundary."
        })json",
                                on_event, "resp_decider");
    }
    if (request.system_prompt == compactor_prompt) {
        return EmitResponseText(R"json({
            "tier1_detail": "Fallback detail.",
            "tier2_summary": "Fallback summary.",
            "tier3_ref": "Fallback ref.",
            "tier3_keywords": ["fallback", "summary", "memory", "test", "compactor"],
            "salience": 5
        })json",
                                on_event, "resp_compactor");
    }
    return EmitResponseText(
        std::string(main_prefix) +
            isla::server::ai_gateway::test::ExtractLatestPromptLine(request.user_text),
        on_event);
}

TEST(EvalRunnerTest, RunsCaseThroughAppBoundaryAndCapturesPromptArtifacts) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "basic_app_boundary",
        .session_id = "eval_session_1",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "nice to meet you",
                },
            },
        .input =
            EvalInput{
                .text = "what did i just say?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    ASSERT_TRUE(artifacts->final_reply.has_value());
    EXPECT_EQ(*artifacts->final_reply, "stub reply: what did i just say?");
    EXPECT_NE(artifacts->prompt.system_prompt.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("] hello"), std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("] nice to meet you"),
              std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("] what did i just say?"),
              std::string::npos);
    EXPECT_TRUE(artifacts->pre_turn_mid_term_episodes.empty());
    EXPECT_TRUE(artifacts->post_turn_mid_term_episodes.empty());
    ASSERT_EQ(artifacts->emitted_events.size(), 2U);
    EXPECT_EQ(artifacts->emitted_events[0].op, "text.output");
    EXPECT_EQ(artifacts->emitted_events[1].op, "turn.completed");
}

TEST(EvalRunnerTest, CapturesStructuredMidTermEpisodesAfterFlushIsApplied) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto decider_call_count = std::make_shared<int>(0);
    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt,
         decider_call_count](const OpenAiResponsesRequest& request,
                             const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.system_prompt == decider_prompt) {
                const int call_index = (*decider_call_count)++;
                if (call_index == 0) {
                    return EmitResponseText(R"json({
                        "should_flush": true,
                        "item_id": "i0",
                        "split_at": null,
                        "reasoning": "Completed first exchange."
                    })json",
                                            on_event, "resp_decider");
                }
                return EmitResponseText(R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No additional completed episode."
                })json",
                                        on_event, "resp_decider");
            }
            if (request.system_prompt == compactor_prompt) {
                return EmitResponseText(R"json({
                    "tier1_detail": "First exchange detail.",
                    "tier2_summary": "First exchange summary.",
                    "tier3_ref": "First exchange ref.",
                    "tier3_keywords": ["first", "exchange", "summary", "memory", "test"],
                    "salience": 8
                })json",
                                        on_event, "resp_compactor");
            }

            return EmitResponseText(
                std::string("stub reply: ") +
                    isla::server::ai_gateway::test::ExtractLatestPromptLine(request.user_text),
                on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "mid_term_snapshot",
        .session_id = "eval_session_2",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "stub reply: hello",
                },
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "tell me something else first",
                },
            },
        .input =
            EvalInput{
                .text = "can you summarize what we were talking about?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_LE(artifacts->pre_turn_mid_term_episodes.size(), 1U);
    if (!artifacts->pre_turn_mid_term_episodes.empty()) {
        EXPECT_EQ(artifacts->pre_turn_mid_term_episodes[0].tier2_summary,
                  "First exchange summary.");
        EXPECT_TRUE(artifacts->pre_turn_mid_term_episodes[0].expandable);
    }
    ASSERT_EQ(artifacts->post_turn_mid_term_episodes.size(), 1U);
    EXPECT_EQ(artifacts->post_turn_mid_term_episodes[0].tier2_summary, "First exchange summary.");
    EXPECT_TRUE(artifacts->post_turn_mid_term_episodes[0].expandable);
}

TEST(EvalRunnerTest, WaitsForDelayedMidTermFlushBeforeCapturingPostTurnSnapshot) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto decider_call_count = std::make_shared<int>(0);
    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt,
         decider_call_count](const OpenAiResponsesRequest& request,
                             const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.system_prompt == decider_prompt) {
                const int call_index = (*decider_call_count)++;
                if (call_index == 0) {
                    return EmitResponseText(R"json({
                        "should_flush": true,
                        "item_id": "i0",
                        "split_at": null,
                        "reasoning": "Completed first exchange."
                    })json",
                                            on_event, "resp_decider");
                }
                return EmitResponseText(R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No additional completed episode."
                })json",
                                        on_event, "resp_decider");
            }
            if (request.system_prompt == compactor_prompt) {
                std::this_thread::sleep_for(50ms);
                return EmitResponseText(R"json({
                    "tier1_detail": "First exchange detail.",
                    "tier2_summary": "First exchange summary.",
                    "tier3_ref": "First exchange ref.",
                    "tier3_keywords": ["first", "exchange", "summary", "memory", "test"],
                    "salience": 8
                })json",
                                        on_event, "resp_compactor");
            }

            return EmitResponseText(
                std::string("stub reply: ") +
                    isla::server::ai_gateway::test::ExtractLatestPromptLine(request.user_text),
                on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 250ms,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "mid_term_snapshot_delayed_compactor",
        .session_id = "eval_session_delayed_mid_term",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "stub reply: hello",
                },
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "tell me something else first",
                },
            },
        .input =
            EvalInput{
                .text = "can you summarize what we were talking about?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    ASSERT_EQ(artifacts->post_turn_mid_term_episodes.size(), 1U);
    EXPECT_EQ(artifacts->post_turn_mid_term_episodes[0].tier2_summary, "First exchange summary.");
    EXPECT_TRUE(artifacts->post_turn_mid_term_episodes[0].expandable);
}

TEST(EvalRunnerTest, ReplaysConversationAssistantMessageWithoutPriorUser) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "assistant_without_prior_user",
        .session_id = "eval_session_assistant_without_prior_user",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "this transcript starts with assistant context",
                },
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "follow-up user question",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "follow-up assistant reply",
                },
            },
        .input =
            EvalInput{
                .text = "what did i just say?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    EXPECT_NE(artifacts->prompt.working_memory_context.find(
                  "this transcript starts with assistant context"),
              std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("follow-up user question"),
              std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("follow-up assistant reply"),
              std::string::npos);
}

TEST(EvalRunnerTest, ReplaysEvalCaseWithEmptyHistoryMessages) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "empty_history_messages",
        .session_id = "eval_session_empty_history_messages",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "",
                },
            },
        .input =
            EvalInput{
                .text = "what did i just say?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    ASSERT_EQ(artifacts->replayed_session_history.size(), 4U);
    EXPECT_EQ(artifacts->replayed_session_history[0].role, std::optional<std::string>("user"));
    EXPECT_EQ(artifacts->replayed_session_history[0].text, std::optional<std::string>(""));
    EXPECT_EQ(artifacts->replayed_session_history[1].role, std::optional<std::string>("assistant"));
    EXPECT_EQ(artifacts->replayed_session_history[1].text, std::optional<std::string>(""));
}

TEST(EvalRunnerTest, ReplaysEvalCaseWithEmptyInput) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .openai_client = client,
            },
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "empty_input",
        .session_id = "eval_session_empty_input",
        .input =
            EvalInput{
                .text = "",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    ASSERT_EQ(artifacts->replayed_session_history.size(), 2U);
    EXPECT_EQ(artifacts->replayed_session_history[0].role, std::optional<std::string>("user"));
    EXPECT_EQ(artifacts->replayed_session_history[0].text, std::optional<std::string>(""));
}

TEST(EvalRunnerTest, BuildsEvalCaseFromBenchmarkTimelineInput) {
    const absl::StatusOr<EvalCase> eval_case =
        BuildEvalCaseFromBenchmarkTimeline(EvalBenchmarkTimelineCase{
            .benchmark_name = "isla_custom_memory",
            .case_id = "timeline_case",
            .session_id = "eval_session_timeline",
            .session_start_time = Ts("2026-03-14T09:59:00Z"),
            .evaluation_reference_time = Ts("2026-03-20T08:00:00Z"),
            .conversation =
                {
                    EvalConversationMessage{
                        .role = MessageRole::User,
                        .text = "hello from setup",
                        .create_time = Ts("2026-03-14T10:00:00Z"),
                    },
                    EvalConversationMessage{
                        .role = MessageRole::Assistant,
                        .text = "setup reply",
                        .create_time = Ts("2026-03-14T10:00:05Z"),
                    },
                },
            .input =
                EvalInput{
                    .text = "evaluate this turn",
                    .create_time = Ts("2026-03-15T11:30:00Z"),
                },
            .expected_answer = std::string("expected answer"),
        });

    ASSERT_TRUE(eval_case.ok()) << eval_case.status();
    EXPECT_EQ(eval_case->benchmark_name, "isla_custom_memory");
    EXPECT_EQ(eval_case->case_id, "timeline_case");
    EXPECT_EQ(eval_case->session_id, "eval_session_timeline");
    ASSERT_EQ(eval_case->conversation.size(), 2U);
    EXPECT_EQ(eval_case->conversation[0].role, MessageRole::User);
    EXPECT_EQ(eval_case->conversation[1].role, MessageRole::Assistant);
    EXPECT_EQ(eval_case->input.text, "evaluate this turn");
    EXPECT_EQ(eval_case->session_start_time, Ts("2026-03-14T09:59:00Z"));
    EXPECT_EQ(eval_case->evaluation_reference_time, Ts("2026-03-20T08:00:00Z"));
    ASSERT_TRUE(eval_case->expected_answer.has_value());
    EXPECT_EQ(*eval_case->expected_answer, "expected answer");
}

TEST(EvalRunnerTest, BuildsBenchmarkTimelineWithAssistantMessageWithoutPriorUser) {
    const absl::StatusOr<EvalCase> eval_case =
        BuildEvalCaseFromBenchmarkTimeline(EvalBenchmarkTimelineCase{
            .benchmark_name = "isla_custom_memory",
            .case_id = "timeline_case_invalid",
            .session_id = "eval_session_timeline_invalid",
            .conversation =
                {
                    EvalConversationMessage{
                        .role = MessageRole::Assistant,
                        .text = "assistant context",
                    },
                },
            .input =
                EvalInput{
                    .text = "evaluate this turn",
                },
        });

    ASSERT_TRUE(eval_case.ok()) << eval_case.status();
    ASSERT_EQ(eval_case->conversation.size(), 1U);
    EXPECT_EQ(eval_case->conversation[0].role, MessageRole::Assistant);
    EXPECT_EQ(eval_case->conversation[0].text, "assistant context");
}

TEST(EvalRunnerTest, UsesBenchmarkSuppliedTimesWithoutInjectingEvalOnlyPromptContext) {
    auto store = std::make_shared<RecordingMemoryStore>();
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .memory_store = store,
                .openai_client = client,
            },
    });

    const Timestamp session_start_time = Ts("2026-03-14T09:59:00Z");
    const Timestamp history_user_time = Ts("2026-03-14T10:00:00Z");
    const Timestamp history_assistant_time = Ts("2026-03-14T10:00:05Z");
    const Timestamp evaluated_user_time = Ts("2026-03-15T11:30:00Z");
    const Timestamp evaluation_reference_time = Ts("2026-03-20T08:00:00Z");

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "benchmark_times",
        .session_id = "eval_session_3",
        .session_start_time = session_start_time,
        .evaluation_reference_time = evaluation_reference_time,
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello from the past",
                    .create_time = history_user_time,
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "explicit benchmark setup reply",
                    .create_time = history_assistant_time,
                },
            },
        .input =
            EvalInput{
                .text = "what time is this benchmark evaluated at?",
                .create_time = evaluated_user_time,
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    ASSERT_EQ(store->session_records.size(), 1U);
    EXPECT_EQ(store->session_records[0].user_id, "eval_isla_custom_memory");
    EXPECT_EQ(store->session_records[0].created_at, session_start_time);
    ASSERT_GE(store->message_writes.size(), 4U);
    EXPECT_EQ(store->message_writes[0].create_time, history_user_time);
    EXPECT_EQ(store->message_writes[1].create_time, history_assistant_time);
    EXPECT_EQ(store->message_writes[2].create_time, evaluated_user_time);
    EXPECT_EQ(artifacts->session_start_time, session_start_time);
    EXPECT_EQ(artifacts->evaluation_reference_time, evaluation_reference_time);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("2026-03-14T10:00:00Z"),
              std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("2026-03-15T11:30:00Z"),
              std::string::npos);
    EXPECT_EQ(artifacts->prompt.working_memory_context.find("<evaluation_reference_time>"),
              std::string::npos);
    EXPECT_EQ(artifacts->prompt.working_memory_context.find("2026-03-20T08:00:00Z"),
              std::string::npos);

    ASSERT_EQ(artifacts->replayed_session_history.size(), 6U);
    EXPECT_EQ(artifacts->replayed_session_history[0].kind, EvalReplayEventKind::kSessionStart);
    EXPECT_EQ(artifacts->replayed_session_history[0].timestamp, session_start_time);

    EXPECT_EQ(artifacts->replayed_session_history[1].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[1].turn_id, std::nullopt);
    EXPECT_EQ(artifacts->replayed_session_history[1].role, std::optional<std::string>("user"));
    EXPECT_EQ(artifacts->replayed_session_history[1].timestamp, history_user_time);
    EXPECT_EQ(artifacts->replayed_session_history[1].text,
              std::optional<std::string>("hello from the past"));

    EXPECT_EQ(artifacts->replayed_session_history[2].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[2].turn_id, std::nullopt);
    EXPECT_EQ(artifacts->replayed_session_history[2].role, std::optional<std::string>("assistant"));
    EXPECT_EQ(artifacts->replayed_session_history[2].timestamp, history_assistant_time);
    EXPECT_EQ(artifacts->replayed_session_history[2].text,
              std::optional<std::string>("explicit benchmark setup reply"));

    EXPECT_EQ(artifacts->replayed_session_history[3].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[3].turn_id,
              std::optional<std::string>("evaluated_turn"));
    EXPECT_EQ(artifacts->replayed_session_history[3].role, std::optional<std::string>("user"));
    EXPECT_EQ(artifacts->replayed_session_history[3].timestamp, evaluated_user_time);
    EXPECT_EQ(artifacts->replayed_session_history[3].text,
              std::optional<std::string>("what time is this benchmark evaluated at?"));

    EXPECT_EQ(artifacts->replayed_session_history[4].kind,
              EvalReplayEventKind::kEvaluationReferenceTime);
    EXPECT_EQ(artifacts->replayed_session_history[4].timestamp, evaluation_reference_time);

    EXPECT_EQ(artifacts->replayed_session_history[5].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[5].turn_id,
              std::optional<std::string>("evaluated_turn"));
    EXPECT_EQ(artifacts->replayed_session_history[5].role, std::optional<std::string>("assistant"));
    EXPECT_EQ(artifacts->replayed_session_history[5].timestamp, std::nullopt);
    EXPECT_EQ(artifacts->replayed_session_history[5].text,
              std::optional<std::string>("stub reply: what time is this benchmark evaluated at?"));
}

TEST(EvalRunnerTest, OrdersEvaluationReferenceTimeBeforeLaterEvaluatedTurnInput) {
    auto store = std::make_shared<RecordingMemoryStore>();
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_prompt = *decider_prompt, compactor_prompt = *compactor_prompt](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            return EmitMidTermAwareReply(decider_prompt, compactor_prompt, request, on_event);
        });

    EvalRunner runner(EvalRunnerConfig{
        .responder_config =
            isla::server::ai_gateway::GatewayStubResponderConfig{
                .response_delay = 0ms,
                .async_emit_timeout = 2s,
                .memory_store = store,
                .openai_client = client,
            },
    });

    const Timestamp session_start_time = Ts("2026-03-14T09:59:00Z");
    const Timestamp history_user_time = Ts("2026-03-14T10:00:00Z");
    const Timestamp evaluated_user_time = Ts("2026-03-20T08:30:00Z");
    const Timestamp evaluation_reference_time = Ts("2026-03-20T08:00:00Z");

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "isla_custom_memory",
        .case_id = "benchmark_times_reference_before_input",
        .session_id = "eval_session_4",
        .session_start_time = session_start_time,
        .evaluation_reference_time = evaluation_reference_time,
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello from setup",
                    .create_time = history_user_time,
                },
            },
        .input =
            EvalInput{
                .text = "what time is this benchmark evaluated at?",
                .create_time = evaluated_user_time,
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    ASSERT_EQ(artifacts->replayed_session_history.size(), 5U);
    EXPECT_EQ(artifacts->replayed_session_history[0].kind, EvalReplayEventKind::kSessionStart);
    EXPECT_EQ(artifacts->replayed_session_history[1].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[1].timestamp, history_user_time);
    EXPECT_EQ(artifacts->replayed_session_history[2].kind,
              EvalReplayEventKind::kEvaluationReferenceTime);
    EXPECT_EQ(artifacts->replayed_session_history[2].timestamp, evaluation_reference_time);
    EXPECT_EQ(artifacts->replayed_session_history[3].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[3].turn_id,
              std::optional<std::string>("evaluated_turn"));
    EXPECT_EQ(artifacts->replayed_session_history[3].timestamp, evaluated_user_time);
    EXPECT_EQ(artifacts->replayed_session_history[4].kind,
              EvalReplayEventKind::kConversationMessage);
    EXPECT_EQ(artifacts->replayed_session_history[4].timestamp, std::nullopt);
}

} // namespace
} // namespace isla::server::evals
