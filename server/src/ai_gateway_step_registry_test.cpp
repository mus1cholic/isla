#include "isla/server/ai_gateway_step_registry.hpp"

#include <memory>

#include <gtest/gtest.h>

#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

TEST(GatewayStepRegistryTest, RejectsMissingConfiguredResponsesClient) {
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
    EXPECT_EQ(result.status().message(), "openai llms requires a configured responses client");
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
    EXPECT_EQ(client->last_request.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(client->last_request.system_prompt, "runtime system");
    EXPECT_EQ(client->last_request.user_text, "hello");
    EXPECT_EQ(client->last_request.reasoning_effort, OpenAiReasoningEffort::kNone);
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

} // namespace
} // namespace isla::server::ai_gateway
