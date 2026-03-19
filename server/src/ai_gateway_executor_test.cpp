#include "isla/server/ai_gateway_executor.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"
#include "openai_responses_client_mock.hpp"

namespace isla::server::ai_gateway {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

void ExpectExecutorTelemetryRecorded(const test::RecordingTelemetrySink& telemetry_sink) {
    const std::vector<test::TelemetryEventRecord> events = telemetry_sink.events();
    const std::vector<test::TelemetryPhaseRecord> phases = telemetry_sink.phases();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].name, telemetry::kEventExecutorStarted);
    EXPECT_EQ(events[1].name, telemetry::kEventExecutorCompleted);
    EXPECT_TRUE(test::ContainsTelemetryPhase(phases, telemetry::kPhaseExecutorTotal));
}

TEST(GatewayPlanExecutorTest, RejectsEmptyPlan) {
    GatewayPlanExecutor executor;
    auto telemetry_sink = std::make_shared<test::RecordingTelemetrySink>();

    const ExecutionOutcome outcome = executor.Execute(
        ExecutionPlan{},
        ExecutionRuntimeInput{
            .user_text = "shared input",
            .telemetry_context = MakeTurnTelemetryContext("srv_test", "turn_empty", telemetry_sink),
        });

    ASSERT_TRUE(std::holds_alternative<ExecutionFailure>(outcome));
    const ExecutionFailure& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_TRUE(failure.step_name.empty());
    EXPECT_EQ(failure.code, "bad_request");
    EXPECT_EQ(failure.message, "execution plan must include at least one step");
    EXPECT_FALSE(failure.retryable);
    ExpectExecutorTelemetryRecorded(*telemetry_sink);
}

TEST(GatewayPlanExecutorTest, ExecutesItemsInOrderAndCollectsResults) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    {
        InSequence sequence;
        EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
        EXPECT_CALL(*client, StreamResponse(_, _))
            .WillOnce([](const OpenAiResponsesRequest& request,
                         const OpenAiResponsesEventCallback& on_event) {
                EXPECT_EQ(request.model, "model_a");
                return test::EmitOpenAiResponse("alpha", on_event, "resp_1");
            });
        EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
        EXPECT_CALL(*client, StreamResponse(_, _))
            .WillOnce([](const OpenAiResponsesRequest& request,
                         const OpenAiResponsesEventCallback& on_event) {
                EXPECT_EQ(request.model, "model_b");
                return test::EmitOpenAiResponse("beta", on_event, "resp_2");
            });
    }
    auto telemetry_sink = std::make_shared<test::RecordingTelemetrySink>();
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_1",
                                    telemetry_sink),
    });

    ASSERT_TRUE(std::holds_alternative<ExecutionResult>(outcome));
    const auto& result = std::get<ExecutionResult>(outcome);
    ASSERT_EQ(result.step_results.size(), 2U);
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[0]).output_text, "alpha");
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[1]).output_text, "beta");
    ExpectExecutorTelemetryRecorded(*telemetry_sink);
}

TEST(GatewayPlanExecutorTest, StopsAtFirstFailingItem) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_EQ(failure.step_name, "step_a");
    EXPECT_EQ(failure.code, "bad_request");
    EXPECT_EQ(failure.message, "execution step rejected the request");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsMissingProviderConfigurationToInternalError) {
    GatewayPlanExecutor executor;

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
    EXPECT_EQ(failure.code, "internal_error");
    EXPECT_EQ(failure.message, "execution step failed");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsInternalStepFailuresToStablePublicError) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([](const OpenAiResponsesRequest& request,
                     const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            static_cast<void>(request);
            static_cast<void>(on_event);
            throw std::runtime_error("boom");
        });
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.failed_step_index, 0U);
    EXPECT_EQ(failure.step_name, "step_a");
    EXPECT_EQ(failure.code, "internal_error");
    EXPECT_EQ(failure.message, "execution step failed");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsUnauthenticatedProviderFailuresToAuthenticationError) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce(Return(absl::UnauthenticatedError("bad api key")));
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "authentication_error");
    EXPECT_EQ(failure.message, "upstream authentication failed");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsPermissionDeniedProviderFailuresToPermissionDenied) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce(Return(absl::PermissionDeniedError("project mismatch")));
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "permission_denied");
    EXPECT_EQ(failure.message, "upstream request was not permitted");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsResourceExhaustedProviderFailuresToResponseTooLarge) {
    auto client = std::make_shared<test::MockOpenAiResponsesClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce(Return(absl::ResourceExhaustedError("too much output")));
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = client,
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "response_too_large");
    EXPECT_EQ(failure.message, "execution step produced too much output");
    EXPECT_FALSE(failure.retryable);
}

} // namespace
} // namespace isla::server::ai_gateway
