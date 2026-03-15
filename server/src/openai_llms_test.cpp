#include "isla/server/openai_llms.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

#include "isla/server/ai_gateway_session_handler.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

TEST(OpenAiReasoningEffortTest, MapsAllSupportedEffortValuesToSchemaStrings) {
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kNone), "none");
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kMinimal), "minimal");
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kLow), "low");
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kMedium), "medium");
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kHigh), "high");
    EXPECT_EQ(OpenAiReasoningEffortToString(OpenAiReasoningEffort::kXHigh), "xhigh");
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

TEST(OpenAiLlmstest, RejectsMissingConfiguredResponsesClient) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hello" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(result.status().message(), "openai llms requires a configured responses client");
}

TEST(OpenAiLlmstest, UsesInjectedOpenAiResponsesClientWhenConfigured) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "hello world");
    OpenAiLLMs openai_llms("main", "system prompt", "gpt-5.4", client,
                           OpenAiReasoningEffort::kMedium);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(client->last_request.model, "gpt-5.4");
    ASSERT_EQ(client->last_request.system_prompt, "system prompt");
    ASSERT_EQ(client->last_request.user_text, "hi");
    EXPECT_EQ(client->last_request.reasoning_effort, OpenAiReasoningEffort::kMedium);
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "hello world");
}

TEST(OpenAiLlmstest, UsesRuntimeSystemPromptOverrideWhenProvided) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "hello world");
    OpenAiLLMs openai_llms("main", "planner prompt", "gpt-5.4", client);

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(
        0, ExecutionRuntimeInput{ .system_prompt = "rendered system prompt", .user_text = "ctx" });

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(client->last_request.system_prompt, "rendered system prompt");
    ASSERT_EQ(client->last_request.user_text, "ctx");
    EXPECT_EQ(client->last_request.reasoning_effort, OpenAiReasoningEffort::kNone);
}

TEST(OpenAiLlmstest, PropagatesInjectedOpenAiResponsesClientFailure) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::UnavailableError("rate limited"));
    OpenAiLLMs openai_llms("main", "", "gpt-5.4", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(result.status().message(), "rate limited");
}

TEST(OpenAiLlmstest, PropagatesInjectedOpenAiResponsesClientValidationFailure) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(), "", "resp_test",
                                                      absl::UnauthenticatedError("bad api key"));
    OpenAiLLMs openai_llms("main", "", "gpt-5.4", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnauthenticated);
    EXPECT_EQ(result.status().message(), "bad api key");
}

TEST(OpenAiLlmstest, ConvertsResponsesClientExceptionToInternalError) {
    auto client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            static_cast<void>(request);
            static_cast<void>(on_event);
            throw std::runtime_error("boom");
        });
    OpenAiLLMs openai_llms("main", "", "gpt-5.4", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hello" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_EQ(result.status().message(), "openai llms response building failed");
}

TEST(OpenAiLlmstest, RejectsProviderOutputThatExceedsMaximumLength) {
    auto client = test::MakeFakeOpenAiResponsesClient(absl::OkStatus(),
                                                      std::string(kMaxTextOutputBytes + 1U, 'x'));
    OpenAiLLMs openai_llms("main", "", "gpt-5.4", client);

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, ExecutionRuntimeInput{ .user_text = "hi" });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(result.status().message(), "openai llms output exceeds maximum length");
}

} // namespace
} // namespace isla::server::ai_gateway
