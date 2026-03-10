#include "isla/server/ai_gateway_executor.hpp"
#include <vector>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

TEST(GatewayPlanExecutorTest, RejectsEmptyPlan) {
    GatewayPlanExecutor executor;

    const ExecutionOutcome outcome = executor.Execute(ExecutionPlan{},
                                                      ExecutionRuntimeInput{
                                                          .user_text = "shared input",
                                                      });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_TRUE(failure.step_name.empty());
    EXPECT_EQ(failure.code, "invalid_request");
    EXPECT_EQ(failure.message, "execution plan must include at least one step");
}

TEST(GatewayPlanExecutorTest, ExecutesItemsInOrderAndCollectsResults) {
    std::vector<std::string> execution_order;
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .response_prefix = "",
        .response_builder =
            [&](std::string_view prefix, std::string_view user_text) -> std::string {
            static_cast<void>(prefix);
            if (execution_order.empty()) {
                execution_order.push_back("item_a");
                return std::string("alpha");
            }
            execution_order.push_back("item_b");
            static_cast<void>(user_text);
            return std::string("beta");
        },
    });

    const ExecutionOutcome outcome = executor.Execute(ExecutionPlan{
        .steps =
            {
                OpenAiLlmStep{
                    .step_name = "step_a",
                    .system_prompt = "",
                    .model = "model_a",
                },
                OpenAiLlmStep{
                    .step_name = "step_b",
                    .system_prompt = "",
                    .model = "model_b",
                },
            },
    },
                                             ExecutionRuntimeInput{
                                                 .user_text = "shared input",
                                             });

    ASSERT_TRUE(std::holds_alternative<ExecutionResult>(outcome));
    EXPECT_EQ(execution_order, std::vector<std::string>({ "item_a", "item_b" }));

    const ExecutionResult& result = std::get<ExecutionResult>(outcome);
    ASSERT_EQ(result.step_results.size(), 2U);
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[0]).output_text, "alpha");
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[1]).output_text, "beta");
}

TEST(GatewayPlanExecutorTest, StopsAtFirstFailingItem) {
    bool second_ran = false;
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .response_prefix = "",
        .response_builder =
            [&](std::string_view prefix, std::string_view user_text) -> std::string {
            static_cast<void>(prefix);
            second_ran = true;
            return std::string(user_text);
        },
    });

    const ExecutionOutcome outcome = executor.Execute(ExecutionPlan{
        .steps =
            {
                OpenAiLlmStep{
                    .step_name = "step_a",
                    .system_prompt = "",
                    .model = "",
                },
                OpenAiLlmStep{
                    .step_name = "step_b",
                    .system_prompt = "",
                    .model = "model_b",
                },
            },
    },
                                             ExecutionRuntimeInput{
                                                 .user_text = "shared input",
                                             });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_EQ(failure.step_name, "step_a");
    EXPECT_EQ(failure.code, "invalid_request");
    EXPECT_EQ(failure.message, "openai llms must include a model");
    EXPECT_FALSE(second_ran);
}

} // namespace
} // namespace isla::server::ai_gateway
