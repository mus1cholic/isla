#include "isla/server/ai_gateway_step_registry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

class FakeLlmClient final : public isla::server::LlmClient {
  public:
    using StreamHandler = std::function<absl::Status(const isla::server::LlmRequest&,
                                                     const isla::server::LlmEventCallback&)>;

    explicit FakeLlmClient(absl::Status status = absl::OkStatus(), std::string full_text = "",
                           StreamHandler stream_handler = {})
        : status_(std::move(status)), full_text_(std::move(full_text)),
          stream_handler_(std::move(stream_handler)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const isla::server::LlmRequest& request,
                   const isla::server::LlmEventCallback& on_event) const override {
        last_request = request;
        if (stream_handler_) {
            return stream_handler_(request, on_event);
        }
        if (!status_.ok()) {
            return status_;
        }
        const absl::Status delta_status = on_event(isla::server::LlmTextDeltaEvent{
            .text_delta = full_text_,
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(isla::server::LlmCompletedEvent{
            .response_id = "resp_test",
        });
    }

    mutable isla::server::LlmRequest last_request;

  private:
    absl::Status status_;
    std::string full_text_;
    StreamHandler stream_handler_;
};

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

TEST(GatewayStepRegistryTest, UsesConfiguredLlmClientWhenPresent) {
    auto client = std::make_shared<FakeLlmClient>(absl::OkStatus(), "provider response");
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
    EXPECT_EQ(client->last_request.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(client->last_request.system_prompt, "runtime system");
    EXPECT_EQ(client->last_request.user_text, "hello");
    EXPECT_EQ(client->last_request.reasoning_effort, isla::server::LlmReasoningEffort::kNone);
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
