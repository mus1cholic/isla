#include "isla/server/ai_gateway_step_registry.hpp"

#include <memory>
#include <string>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"
#include "llm_client_mock.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

using ::testing::_;
using ::testing::Return;

TEST(GatewayStepRegistryTest, RejectsMissingConfiguredLlmClient) {
    GatewayStepRegistry registry;

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "main",
                                 .system_prompt = "",
                                 .model = "gpt-4.1-mini",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "",
                                 .user_text = "hello",
                             });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(result.status().message(), "openai llms requires a configured llm client");
}

TEST(GatewayStepRegistryTest, ReportsStepNameForOpenAiStep) {
    GatewayStepRegistry registry;

    EXPECT_EQ(registry.StepName(ExecutionStep(OpenAiLlmStep{
                  .step_name = "main",
                  .system_prompt = "",
                  .model = "gpt-4.1-mini",
              })),
              "main");
}

TEST(GatewayStepRegistryTest, RejectsMissingRuntimeUserText) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "ignored");
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "main",
                                 .system_prompt = "",
                                 .model = "gpt-4.1-mini",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "",
                                 .user_text = "",
                             });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "openai llms input must include user_text");
}

TEST(GatewayStepRegistryTest, UsesConfiguredOpenAiResponsesClientWhenPresent) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "provider response");
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "main",
                                 .system_prompt = "system",
                                 .model = "gpt-5.3-chat-latest",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "runtime system",
                                 .user_text = "hello",
                             });

    ASSERT_TRUE(result.ok()) << result.status();
    const OpenAiResponsesRequest last_request = client->last_request_snapshot();
    EXPECT_EQ(last_request.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(last_request.system_prompt, "runtime system");
    EXPECT_EQ(last_request.user_text, "hello");
    EXPECT_EQ(last_request.reasoning_effort, OpenAiReasoningEffort::kNone);
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

TEST(GatewayStepRegistryTest, OverridesPlannerSelectedModelWhenRuntimeConfigIsSet) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "provider response");
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .llm_runtime_config =
            GatewayLlmRuntimeConfig{
                .main_model = "gpt-4.1-mini",
            },
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "main",
                                 .system_prompt = "system",
                                 .model = "gpt-5.3-chat-latest",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "runtime system",
                                 .user_text = "hello",
                             });

    ASSERT_TRUE(result.ok()) << result.status();
    const OpenAiResponsesRequest last_request = client->last_request_snapshot();
    EXPECT_EQ(last_request.model, "gpt-4.1-mini");
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

TEST(GatewayStepRegistryTest, LeavesNonMainStepModelUnchangedWhenMainOverrideIsSet) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "provider response");
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .llm_runtime_config =
            GatewayLlmRuntimeConfig{
                .main_model = "gpt-4.1-mini",
            },
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "summary",
                                 .system_prompt = "system",
                                 .model = "gpt-4.1-nano",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "runtime system",
                                 .user_text = "hello",
                             });

    ASSERT_TRUE(result.ok()) << result.status();
    const OpenAiResponsesRequest last_request = client->last_request_snapshot();
    EXPECT_EQ(last_request.model, "gpt-4.1-nano");
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

TEST(GatewayStepRegistryTest, UsesConfiguredLlmClientWhenPresent) {
    auto client = std::make_shared<isla::server::test::MockLlmClient>();
    isla::server::LlmRequest captured_request;
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([&captured_request](const isla::server::LlmRequest& request,
                                      const isla::server::LlmEventCallback& on_event) {
            captured_request = request;
            const absl::Status delta_status = on_event(isla::server::LlmTextDeltaEvent{
                .text_delta = "provider response",
            });
            if (!delta_status.ok()) {
                return delta_status;
            }
            return on_event(isla::server::LlmCompletedEvent{
                .response_id = "resp_test",
            });
        });
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .llm_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result =
        registry.ExecuteStep(0,
                             ExecutionStep(OpenAiLlmStep{
                                 .step_name = "main",
                                 .system_prompt = "system",
                                 .model = "gpt-5.3-chat-latest",
                             }),
                             ExecutionRuntimeInput{
                                 .system_prompt = "runtime system",
                                 .user_text = "hello",
                             });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(captured_request.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(captured_request.system_prompt, "runtime system");
    EXPECT_EQ(captured_request.user_text, "hello");
    EXPECT_EQ(captured_request.reasoning_effort, isla::server::LlmReasoningEffort::kNone);
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

TEST(GatewayStepRegistryTest, RecordsExecutorStepPhaseWhenTelemetryContextPresent) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "provider response");
    auto telemetry_sink = std::make_shared<test::RecordingTelemetrySink>();
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "system",
            .model = "gpt-5.3-chat-latest",
        }),
        ExecutionRuntimeInput{
            .system_prompt = "runtime system",
            .user_text = "hello",
            .telemetry_context = MakeTurnTelemetryContext("srv_test", "turn_1", telemetry_sink),
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(
        test::ContainsTelemetryPhase(telemetry_sink->phases(), telemetry::kPhaseExecutorStep));
}

} // namespace
} // namespace isla::server::ai_gateway
