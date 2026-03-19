#include "isla/server/openai_llms.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ai_gateway_telemetry_test_utils.hpp"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "llm_client_mock.hpp"

namespace isla::server::ai_gateway {
namespace {

using ::testing::_;
using ::testing::Return;

std::shared_ptr<isla::server::test::MockLlmClient> MakeMockLlmClient() {
    auto client = std::make_shared<isla::server::test::MockLlmClient>();
    EXPECT_CALL(*client, WarmUp()).Times(0);
    return client;
}

TEST(OpenAiReasoningEffortTest, MapsAllSupportedEffortValuesToSchemaStrings) {
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kNone).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kNone), "none");
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kMinimal).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kMinimal), "minimal");
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kLow).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kLow), "low");
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kMedium).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kMedium), "medium");
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kHigh).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kHigh), "high");
    ASSERT_TRUE(TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kXHigh).has_value());
    EXPECT_EQ(*TryOpenAiReasoningEffortToString(OpenAiReasoningEffort::kXHigh), "xhigh");
}

TEST(OpenAiReasoningEffortTest, ReturnsNulloptForUnknownEffortValue) {
    EXPECT_FALSE(
        TryOpenAiReasoningEffortToString(static_cast<OpenAiReasoningEffort>(999)).has_value());
}

TEST(OpenAiLlmstest, RejectsInvalidReasoningEffort) {
    auto client = MakeMockLlmClient();
    OpenAiLLMs openai_llms("main", "system prompt", "gpt-5.3-chat-latest", client,
                           static_cast<OpenAiReasoningEffort>(999));

    const absl::Status status = openai_llms.Validate();

    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "openai llms reasoning_effort is invalid");
}

TEST(OpenAiLlmstest, RejectsMissingStepName) {
    OpenAiLLMs openai_llms("", "", "gpt-4.1-mini");

    const absl::Status status = openai_llms.Validate();

    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "openai llms must include a step_name");
}

TEST(OpenAiLlmstest, RejectsMissingModel) {
    OpenAiLLMs openai_llms("main", "", "");

    const absl::Status status = openai_llms.Validate();

    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "openai llms must include a model");
}

TEST(OpenAiLlmstest, RejectsMissingUserText) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "openai llms input must include user_text");
}

TEST(OpenAiLlmstest, RejectsMissingConfiguredLlmClient) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hello" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(result.status().message(), "openai llms requires a configured llm client");
}

TEST(OpenAiLlmstest, UsesInjectedLlmClientWhenConfigured) {
    auto client = MakeMockLlmClient();
    isla::server::LlmRequest captured_request;
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([&captured_request](const isla::server::LlmRequest& request,
                                      const isla::server::LlmEventCallback& on_event) {
            captured_request = request;
            return isla::server::test::EmitLlmResponse("hello world", on_event);
        });
    OpenAiLLMs openai_llms("main", "system prompt", "gpt-5.3-chat-latest", client,
                           OpenAiReasoningEffort::kMedium);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(captured_request.model, "gpt-5.3-chat-latest");
    ASSERT_EQ(captured_request.system_prompt, "system prompt");
    ASSERT_EQ(captured_request.user_text, "hi");
    EXPECT_EQ(captured_request.reasoning_effort, isla::server::LlmReasoningEffort::kMedium);
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "hello world");
}

TEST(OpenAiLlmstest, UsesRuntimeSystemPromptOverrideWhenProvided) {
    auto client = MakeMockLlmClient();
    isla::server::LlmRequest captured_request;
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([&captured_request](const isla::server::LlmRequest& request,
                                      const isla::server::LlmEventCallback& on_event) {
            captured_request = request;
            return isla::server::test::EmitLlmResponse("hello world", on_event);
        });
    OpenAiLLMs openai_llms("main", "planner prompt", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(
        0, ExecutionRuntimeInput{ .system_prompt = "rendered system prompt", .user_text = "ctx" });

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(captured_request.system_prompt, "rendered system prompt");
    ASSERT_EQ(captured_request.user_text, "ctx");
    EXPECT_EQ(captured_request.reasoning_effort, isla::server::LlmReasoningEffort::kNone);
}

TEST(OpenAiLlmstest, PropagatesTelemetryContextToLlmClient) {
    auto client = MakeMockLlmClient();
    isla::server::LlmRequest captured_request;
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([&captured_request](const isla::server::LlmRequest& request,
                                      const isla::server::LlmEventCallback& on_event) {
            captured_request = request;
            return isla::server::test::EmitLlmResponse("hello world", on_event);
        });
    OpenAiLLMs openai_llms("main", "planner prompt", "gpt-5.3-chat-latest", client);
    const std::shared_ptr<const TurnTelemetryContext> telemetry_context =
        MakeTurnTelemetryContext("srv_test", "turn_telemetry");

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{
                                           .system_prompt = "rendered system prompt",
                                           .user_text = "ctx",
                                           .telemetry_context = telemetry_context,
                                       });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(captured_request.telemetry_context, telemetry_context);
}

TEST(OpenAiLlmstest, RecordsProviderTotalAndAggregationPhases) {
    auto client = MakeMockLlmClient();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([](const isla::server::LlmRequest& request,
                     const isla::server::LlmEventCallback& on_event) {
            static_cast<void>(request);
            return isla::server::test::EmitLlmResponse("hello world", on_event);
        });
    auto telemetry_sink = std::make_shared<test::RecordingTelemetrySink>();
    OpenAiLLMs openai_llms("main", "planner prompt", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{
                                           .system_prompt = "rendered system prompt",
                                           .user_text = "ctx",
                                           .telemetry_context = MakeTurnTelemetryContext(
                                               "srv_test", "turn_telemetry", telemetry_sink),
                                       });

    ASSERT_TRUE(result.ok()) << result.status();
    const std::vector<test::TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const auto aggregate_phase_count = static_cast<std::size_t>(
        std::count_if(phases.begin(), phases.end(), [](const test::TelemetryPhaseRecord& phase) {
            return phase.name == telemetry::kPhaseProviderAggregateText;
        }));
    EXPECT_TRUE(test::ContainsTelemetryPhase(phases, telemetry::kPhaseLlmProviderTotal));
    EXPECT_TRUE(test::ContainsTelemetryPhase(phases, telemetry::kPhaseProviderAggregateText));
    EXPECT_EQ(aggregate_phase_count, 1U);
}

TEST(OpenAiLlmstest, PropagatesInjectedLlmClientFailure) {
    auto client = MakeMockLlmClient();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce(Return(absl::UnavailableError("rate limited")));
    OpenAiLLMs openai_llms("main", "", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(result.status().message(), "rate limited");
}

TEST(OpenAiLlmstest, PropagatesInjectedLlmClientValidationFailure) {
    auto client = MakeMockLlmClient();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::UnauthenticatedError("bad api key")));
    OpenAiLLMs openai_llms("main", "", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnauthenticated);
    EXPECT_EQ(result.status().message(), "bad api key");
}

TEST(OpenAiLlmstest, ConvertsLlmClientExceptionToInternalError) {
    auto client = MakeMockLlmClient();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([](const isla::server::LlmRequest& request,
                     const isla::server::LlmEventCallback& on_event) -> absl::Status {
            static_cast<void>(request);
            static_cast<void>(on_event);
            throw std::runtime_error("boom");
        });
    OpenAiLLMs openai_llms("main", "", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hello" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_EQ(result.status().message(), "openai llms response building failed");
}

TEST(OpenAiLlmstest, RejectsProviderOutputThatExceedsMaximumLength) {
    auto client = MakeMockLlmClient();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, StreamResponse(_, _))
        .WillOnce([](const isla::server::LlmRequest& request,
                     const isla::server::LlmEventCallback& on_event) {
            static_cast<void>(request);
            return isla::server::test::EmitLlmResponse(std::string(kMaxTextOutputBytes + 1U, 'x'),
                                                       on_event);
        });
    OpenAiLLMs openai_llms("main", "", "gpt-5.3-chat-latest", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(result.status().message(), "openai llms output exceeds maximum length");
}

} // namespace
} // namespace isla::server::ai_gateway
