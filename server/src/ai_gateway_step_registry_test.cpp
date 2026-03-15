#include "isla/server/ai_gateway_step_registry.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        static_cast<void>(context);
        phases.push_back(PhaseRecord{
            .name = std::string(phase_name),
            .started_at = started_at,
            .completed_at = completed_at,
        });
    }

    struct PhaseRecord {
        std::string name;
        TurnTelemetryContext::Clock::time_point started_at;
        TurnTelemetryContext::Clock::time_point completed_at;
    };

    mutable std::vector<PhaseRecord> phases;
};

bool ContainsTelemetryPhase(const std::vector<RecordingTelemetrySink::PhaseRecord>& phases,
                            std::string_view phase_name) {
    for (const auto& phase : phases) {
        if (phase.name == phase_name) {
            return true;
        }
    }
    return false;
}

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

TEST(GatewayStepRegistryTest, RecordsExecutorStepPhaseWhenTelemetryContextPresent) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "provider response");
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
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
    EXPECT_TRUE(ContainsTelemetryPhase(telemetry_sink->phases, telemetry::kPhaseExecutorStep));
}

} // namespace
} // namespace isla::server::ai_gateway
