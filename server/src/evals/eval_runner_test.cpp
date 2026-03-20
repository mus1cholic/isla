#include "isla/server/evals/eval_runner.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "isla/server/memory/prompt_loader.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using namespace std::chrono_literals;

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
        .setup_turns =
            {
                EvalTurnInput{
                    .turn_id = "turn_1",
                    .user_text = "hello",
                },
            },
        .evaluated_turn =
            EvalTurnInput{
                .turn_id = "turn_2",
                .user_text = "what did i just say?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    ASSERT_TRUE(artifacts->final_reply.has_value());
    EXPECT_EQ(*artifacts->final_reply, "stub reply: what did i just say?");
    EXPECT_NE(artifacts->prompt.system_prompt.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("] hello"), std::string::npos);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("] stub reply: hello"),
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
        .setup_turns =
            {
                EvalTurnInput{
                    .turn_id = "turn_1",
                    .user_text = "hello",
                },
            },
        .evaluated_turn =
            EvalTurnInput{
                .turn_id = "turn_2",
                .user_text = "can you summarize what we were talking about?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_TRUE(artifacts->pre_turn_mid_term_episodes.empty());
    ASSERT_EQ(artifacts->post_turn_mid_term_episodes.size(), 1U);
    EXPECT_EQ(artifacts->post_turn_mid_term_episodes[0].tier2_summary, "First exchange summary.");
    EXPECT_TRUE(artifacts->post_turn_mid_term_episodes[0].expandable);
    EXPECT_NE(artifacts->prompt.working_memory_context.find("First exchange summary."),
              std::string::npos);
}

} // namespace
} // namespace isla::server::evals
