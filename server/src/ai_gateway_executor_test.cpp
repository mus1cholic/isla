#include "isla/server/ai_gateway_executor.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

void ExpectExecutorTelemetryRecorded(const test::RecordingTelemetrySink& telemetry_sink) {
    const std::vector<test::TelemetryEventRecord> events = telemetry_sink.events();
    const std::vector<test::TelemetryPhaseRecord> phases = telemetry_sink.phases();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].name, telemetry::kEventExecutorStarted);
    EXPECT_EQ(events[1].name, telemetry::kEventExecutorCompleted);
    EXPECT_TRUE(test::ContainsTelemetryPhase(phases, telemetry::kPhaseExecutorTotal));
}

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

class SequencedOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit SequencedOpenAiResponsesClient(std::vector<std::string> outputs)
        : outputs_(std::move(outputs)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        call_order.push_back(request.model);
        if (next_index_ >= outputs_.size()) {
            return absl::InternalError("unexpected extra provider call");
        }
        const std::string& output = outputs_[next_index_++];
        absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = output });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = "resp_" + std::to_string(next_index_),
        });
    }

    mutable std::vector<std::string> call_order;

  private:
    std::vector<std::string> outputs_;
    mutable std::size_t next_index_ = 0;
};

class ThrowingOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        static_cast<void>(request);
        static_cast<void>(on_event);
        throw std::runtime_error("boom");
    }
};

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
    auto client = std::make_shared<SequencedOpenAiResponsesClient>(
        std::vector<std::string>{ "alpha", "beta" });
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
    EXPECT_EQ(client->call_order, std::vector<std::string>({ "model_a", "model_b" }));

    const auto& result = std::get<ExecutionResult>(outcome);
    ASSERT_EQ(result.step_results.size(), 2U);
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[0]).output_text, "alpha");
    EXPECT_EQ(std::get<LlmCallResult>(result.step_results[1]).output_text, "beta");
    ExpectExecutorTelemetryRecorded(*telemetry_sink);
}

TEST(GatewayPlanExecutorTest, StopsAtFirstFailingItem) {
    auto client = std::make_shared<SequencedOpenAiResponsesClient>(
        std::vector<std::string>{ "alpha", "beta" });
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
    EXPECT_TRUE(client->call_order.empty());
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
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = std::make_shared<ThrowingOpenAiResponsesClient>(),
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
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
    const auto& failure = std::get<ExecutionFailure>(outcome);
    EXPECT_EQ(failure.code, "permission_denied");
    EXPECT_EQ(failure.message, "upstream request was not permitted");
    EXPECT_FALSE(failure.retryable);
}

TEST(GatewayPlanExecutorTest, MapsResourceExhaustedProviderFailuresToResponseTooLarge) {
    GatewayPlanExecutor executor(GatewayStepRegistryConfig{
        .openai_client = std::make_shared<FailingOpenAiResponsesClient>(
            absl::ResourceExhaustedError("too much output")),
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
