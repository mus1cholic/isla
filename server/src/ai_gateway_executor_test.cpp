#include "isla/server/ai_gateway_executor.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

class FailingOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit FailingOpenAiResponsesClient(absl::Status status) : status_(std::move(status)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        static_cast<void>(request);
        static_cast<void>(on_event);
        return status_;
    }

  private:
    absl::Status status_;
};

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
    EXPECT_EQ(failure.code, "bad_request");
    EXPECT_EQ(failure.message, "execution plan must include at least one step");
    EXPECT_FALSE(failure.retryable);
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
    EXPECT_EQ(failure.code, "bad_request");
    EXPECT_EQ(failure.message, "execution step rejected the request");
    EXPECT_FALSE(failure.retryable);
    EXPECT_FALSE(second_ran);
}

TEST(GatewayPlanExecutorTest, MapsInternalStepFailuresToStablePublicError) {
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .response_prefix = "",
        .response_builder =
            [](std::string_view prefix, std::string_view user_text) -> std::string {
            static_cast<void>(prefix);
            static_cast<void>(user_text);
            throw std::runtime_error("boom");
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
            },
    },
                                             ExecutionRuntimeInput{
                                                 .user_text = "shared input",
                                             });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_EQ(failure.step_name, "step_a");
    EXPECT_EQ(failure.code, "internal_error");
    EXPECT_EQ(failure.message, "execution step failed");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsUnauthenticatedProviderFailuresToAuthenticationError) {
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = std::make_shared<FailingOpenAiResponsesClient>(
            absl::UnauthenticatedError("bad api key")),
    });

    const ExecutionOutcome outcome = executor.Execute(ExecutionPlan{
        .steps =
            {
                OpenAiLlmStep{
                    .step_name = "step_a",
                    .system_prompt = "",
                    .model = "model_a",
                },
            },
    },
                                             ExecutionRuntimeInput{
                                                 .user_text = "shared input",
                                             });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "authentication_error");
    EXPECT_EQ(failure.message, "upstream authentication failed");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsPermissionDeniedProviderFailuresToPermissionDenied) {
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = std::make_shared<FailingOpenAiResponsesClient>(
            absl::PermissionDeniedError("project mismatch")),
    });

    const ExecutionOutcome outcome = executor.Execute(ExecutionPlan{
        .steps =
            {
                OpenAiLlmStep{
                    .step_name = "step_a",
                    .system_prompt = "",
                    .model = "model_a",
                },
            },
    },
                                             ExecutionRuntimeInput{
                                                 .user_text = "shared input",
                                             });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "permission_denied");
    EXPECT_EQ(failure.message, "upstream request was not permitted");
    EXPECT_FALSE(failure.retryable);
}

} // namespace
} // namespace isla::server::ai_gateway
