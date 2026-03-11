#include "isla/server/ai_gateway_step_registry.hpp"

#include <memory>
#include <stdexcept>
#include <type_traits>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        last_request = request;
        const absl::Status first_status =
            on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = "provider " });
        if (!first_status.ok()) {
            return first_status;
        }
        const absl::Status second_status =
            on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = "response" });
        if (!second_status.ok()) {
            return second_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{ .response_id = "resp_test" });
    }

    mutable OpenAiResponsesRequest last_request;
};

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
    EXPECT_EQ(result.status().message(), "openai llms response building failed");
}

TEST(GatewayStepRegistryTest, UsesConfiguredOpenAiResponsesClientWhenPresent) {
    auto client = std::make_shared<FakeOpenAiResponsesClient>();
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .response_prefix = "stub echo: ",
        .response_builder = {},
        .openai_config = OpenAiResponsesClientConfig{},
        .openai_client = client,
    });

    const absl::StatusOr<ExecutionStepResult> result = registry.ExecuteStep(
        0,
        ExecutionStep(OpenAiLlmStep{
            .step_name = "main",
            .system_prompt = "system",
            .model = "gpt-5.2",
        }),
        ExecutionRuntimeInput{
            .user_text = "hello",
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(client->last_request.model, "gpt-5.2");
    EXPECT_EQ(client->last_request.system_prompt, "system");
    EXPECT_EQ(client->last_request.user_text, "hello");
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "provider response");
}

} // namespace
} // namespace isla::server::ai_gateway
