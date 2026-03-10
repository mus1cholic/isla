#include "isla/server/ai_gateway_executor.hpp"
#include "isla/server/ai_gateway_planner.hpp"

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

TEST(AiGatewayPlannerTest, CreateFakeOpenAiPlanBuildsOneExecutableItem) {
    const absl::StatusOr<ExecutionPlan> planned = CreateFakeOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();
    ASSERT_EQ(planned->steps.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<OpenAiLlmStep>(planned->steps.front()));
    const OpenAiLlmStep& openai_step = std::get<OpenAiLlmStep>(planned->steps.front());
    EXPECT_EQ(openai_step.step_name, "main");
    EXPECT_EQ(openai_step.model, "gpt-5.2");
}

TEST(AiGatewayPlannerTest, CreateFakeOpenAiPlanDoesNotRequireRuntimeUserText) {
    const absl::StatusOr<ExecutionPlan> planned = CreateFakeOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();
}

TEST(AiGatewayPlannerTest, ExecutorRejectsMissingRuntimeUserText) {
    const absl::StatusOr<ExecutionPlan> planned = CreateFakeOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();

    GatewayPlanExecutor executor;
    const ExecutionOutcome outcome = executor.Execute(*planned, ExecutionRuntimeInput{
                                                                   .user_text = "",
                                                               });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    EXPECT_EQ(std::get<ExecutionFailure>(outcome).code, "invalid_request");
}

TEST(AiGatewayPlannerTest, CreateFakeOpenAiPlanRegistersOpenAiCallShape) {
    const absl::StatusOr<ExecutionPlan> planned = CreateFakeOpenAiPlan();

    ASSERT_TRUE(planned.ok()) << planned.status();
    ASSERT_EQ(planned->steps.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<OpenAiLlmStep>(planned->steps.front()));

    const OpenAiLlmStep& openai_step = std::get<OpenAiLlmStep>(planned->steps.front());
    EXPECT_EQ(openai_step.step_name, "main");
    EXPECT_EQ(openai_step.model, "gpt-5.2");
}

} // namespace
} // namespace isla::server::ai_gateway
