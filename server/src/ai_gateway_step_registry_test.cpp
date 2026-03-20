#include "isla/server/ai_gateway_step_registry.hpp"

#include <memory>
#include <string>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"
#include "isla/server/tools/expand_mid_term_tool.hpp"
#include "llm_client_mock.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

using ::testing::_;
using ::testing::Return;

class StaticToolSessionReader final : public isla::server::tools::ToolSessionReader {
  public:
    [[nodiscard]] absl::StatusOr<std::string>
    ExpandMidTermEpisode(std::string_view episode_id) const override {
        last_episode_id = std::string(episode_id);
        if (episode_id == "ep_123") {
            return std::string("expanded tier1 detail");
        }
        return absl::NotFoundError("mid-term episode was not found");
    }

    mutable std::string last_episode_id;
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

TEST(GatewayStepRegistryTest, ExecutesToolLoopWhenToolContextIsPresent) {
    auto tool_registry = isla::server::tools::ToolRegistry::Create(
        { std::make_shared<const isla::server::tools::ExpandMidTermTool>() });
    ASSERT_TRUE(tool_registry.ok()) << tool_registry.status();
    auto reader = std::make_shared<StaticToolSessionReader>();
    auto request_count = std::make_shared<int>(0);
    auto client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [request_count](const OpenAiResponsesRequest& request,
                        const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            ++(*request_count);
            if (*request_count == 1) {
                EXPECT_EQ(request.user_text, "hello");
                EXPECT_EQ(request.function_tools.size(), 1U);
                EXPECT_EQ(request.function_tools[0].name, "expand_mid_term");
                EXPECT_TRUE(request.input_items.empty());
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_tool_1",
                    .output_items =
                        {
                            OpenAiResponsesOutputItem{
                                .type = "function_call",
                                .raw_json = R"json({"type":"function_call","call_id":"call_1","name":"expand_mid_term","arguments":"{\"episode_id\":\"ep_123\"}"})json",
                                .call_id = std::string("call_1"),
                                .name = std::string("expand_mid_term"),
                                .arguments_json = std::string(R"json({"episode_id":"ep_123"})json"),
                            },
                        },
                });
            }
            EXPECT_TRUE(request.user_text.empty());
            EXPECT_EQ(request.input_items.size(), 2U);
            EXPECT_TRUE(
                std::holds_alternative<OpenAiResponsesRawInputItem>(request.input_items[0]));
            EXPECT_TRUE(std::holds_alternative<OpenAiResponsesFunctionCallOutputInputItem>(
                request.input_items[1]));
            const auto& tool_output =
                std::get<OpenAiResponsesFunctionCallOutputInputItem>(request.input_items[1]);
            EXPECT_EQ(tool_output.call_id, "call_1");
            EXPECT_EQ(tool_output.output, "expanded tier1 detail");
            const absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = "tool-backed response",
            });
            if (!delta_status.ok()) {
                return delta_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_tool_2",
            });
        });
    GatewayStepRegistry registry(GatewayStepRegistryConfig{
        .openai_client = client,
        .tool_registry = std::make_shared<const isla::server::tools::ToolRegistry>(*tool_registry),
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
                                 .tool_execution_context =
                                     isla::server::tools::ToolExecutionContext{
                                         .session_id = "srv_test",
                                         .telemetry_context = nullptr,
                                         .session_reader = reader,
                                     },
                             });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "tool-backed response");
    EXPECT_EQ(reader->last_episode_id, "ep_123");
    EXPECT_EQ(*request_count, 2);
}

} // namespace
} // namespace isla::server::ai_gateway
