#include "isla/server/ai_gateway_executor.hpp"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/memory/prompt_loader.hpp"

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

TEST(AiGatewayPlannerTest, CreateOpenAiPlanBuildsOneExecutableItem) {
    const absl::StatusOr<ExecutionPlan> planned = CreateOpenAiPlan();
    const absl::StatusOr<std::string> system_prompt = isla::server::memory::LoadSystemPrompt();

    ASSERT_TRUE(planned.ok()) << planned.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    ASSERT_EQ(planned->steps.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<OpenAiLlmStep>(planned->steps.front()));
    const auto& openai_step = std::get<OpenAiLlmStep>(planned->steps.front());
    EXPECT_EQ(openai_step.step_name, "main");
    EXPECT_EQ(openai_step.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(openai_step.system_prompt, *system_prompt);
    EXPECT_EQ(openai_step.reasoning_effort, OpenAiReasoningEffort::kMedium);
}

TEST(AiGatewayPlannerTest, CreateOpenAiPlanDoesNotRequireRuntimeUserText) {
    const absl::StatusOr<ExecutionPlan> planned = CreateOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();
}

TEST(AiGatewayPlannerTest, ExecutorRejectsMissingRuntimeUserText) {
    const absl::StatusOr<ExecutionPlan> planned = CreateOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();

    GatewayPlanExecutor executor;
    const ExecutionOutcome outcome = executor.Execute(*planned, ExecutionRuntimeInput{
                                                                    .user_text = "",
                                                                });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    EXPECT_EQ(std::get<ExecutionFailure>(outcome).code, "bad_request");
}

} // namespace
} // namespace isla::server::ai_gateway
