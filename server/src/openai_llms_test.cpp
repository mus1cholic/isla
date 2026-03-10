#include "isla/server/openai_llms.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

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
        openai_llms.GenerateContent(0, "", "stub echo: ", {});

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "openai llms input must include user_text");
}

TEST(OpenAiLlmstest, UsesDefaultPrefixResponseWhenBuilderMissing) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result =
        openai_llms.GenerateContent(0, "hello", "stub echo: ", {});

    ASSERT_TRUE(result.ok()) << result.status();
    const LlmCallResult& llm_result = std::get<LlmCallResult>(*result);
    EXPECT_EQ(llm_result.step_name, "main");
    EXPECT_EQ(llm_result.model, "gpt-4.1-mini");
    EXPECT_EQ(llm_result.output_text, "stub echo: hello");
}

TEST(OpenAiLlmstest, UsesCustomResponseBuilderWhenProvided) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(
        0, "hello", "stub echo: ",
        [](std::string_view prefix, std::string_view user_text) {
            return std::string(prefix) + "<" + std::string(user_text) + ">";
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "stub echo: <hello>");
}

TEST(OpenAiLlmstest, ConvertsBuilderExceptionToInternalError) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(
        0, "hello", "stub echo: ",
        [](std::string_view prefix, std::string_view user_text) -> std::string {
            static_cast<void>(prefix);
            static_cast<void>(user_text);
            throw std::runtime_error("boom");
        });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_EQ(result.status().message(), "stub responder processing failed");
}

} // namespace
} // namespace isla::server::ai_gateway
