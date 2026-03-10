#include "isla/server/ai_gateway_step_registry.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

TEST(GatewayStepRegistryTest, ExecutesOpenAiLlmStep) {
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .response_prefix = "stub echo: ",
        .response_builder = {},
    });

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "",
            .model = "gpt-4.1-mini",
        }),
        ExecutionRuntimeInput{
            .user_text = "hello",
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(std::get<LlmCallResult>(*result).step_name, "main");
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "stub echo: hello");
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

TEST(GatewayStepRegistryTest, ForwardsConfiguredResponseBuilder) {
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .response_prefix = "stub prefix: ",
        .response_builder = [](std::string_view prefix, std::string_view user_text) {
            return std::string(prefix) + "[" + std::string(user_text) + "]";
        },
    });

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "",
            .model = "gpt-4.1-mini",
        }),
        ExecutionRuntimeInput{
            .user_text = "hello",
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "stub prefix: [hello]");
}

TEST(GatewayStepRegistryTest, RejectsMissingRuntimeUserText) {
    GatewayStepRegistry registry;

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "",
            .model = "gpt-4.1-mini",
        }),
        ExecutionRuntimeInput{
            .user_text = "",
        });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "openai llms input must include user_text");
}

TEST(GatewayStepRegistryTest, ConvertsBuilderExceptionToInternalError) {
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .response_prefix = "stub echo: ",
        .response_builder = [](std::string_view prefix, std::string_view user_text) -> std::string {
            static_cast<void>(prefix);
            static_cast<void>(user_text);
            throw std::runtime_error("boom");
        },
    });

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "",
            .model = "gpt-4.1-mini",
        }),
        ExecutionRuntimeInput{
            .user_text = "hello",
        });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_EQ(result.status().message(), "stub responder processing failed");
}

} // namespace
} // namespace isla::server::ai_gateway
