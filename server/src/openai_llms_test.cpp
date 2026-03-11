#include "isla/server/openai_llms.hpp"

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit FakeOpenAiResponsesClient(absl::Status status = absl::OkStatus(),
                                       std::string full_text = "")
        : status_(std::move(status)), full_text_(std::move(full_text)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        last_request = request;
        if (!status_.ok()) {
            return status_;
        }
        const auto midpoint = full_text_.size() / 2U;
        const absl::Status first_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = full_text_.substr(0, midpoint),
        });
        if (!first_status.ok()) {
            return first_status;
        }
        const absl::Status second_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = full_text_.substr(midpoint),
        });
        if (!second_status.ok()) {
            return second_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = "resp_test",
        });
    }

    mutable OpenAiResponsesRequest last_request;

  private:
    absl::Status status_;
    std::string full_text_;
};

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

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.status().message(), "openai llms input must include user_text");
}

TEST(OpenAiLlmstest, UsesDefaultPrefixResponseWhenBuilderMissing) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini");

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "hello");

    ASSERT_TRUE(result.ok()) << result.status();
    const LlmCallResult& llm_result = std::get<LlmCallResult>(*result);
    EXPECT_EQ(llm_result.step_name, "main");
    EXPECT_EQ(llm_result.model, "gpt-4.1-mini");
    EXPECT_EQ(llm_result.output_text, "stub echo: hello");
}

TEST(OpenAiLlmstest, UsesCustomResponseBuilderWhenProvided) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini", nullptr,
                           "stub echo: ", [](std::string_view prefix, std::string_view user_text) {
                               return std::string(prefix) + "<" + std::string(user_text) + ">";
                           });

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "hello");

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "stub echo: <hello>");
}

TEST(OpenAiLlmstest, ConvertsBuilderExceptionToInternalError) {
    OpenAiLLMs openai_llms("main", "", "gpt-4.1-mini", nullptr, "stub echo: ",
                           [](std::string_view prefix, std::string_view user_text) -> std::string {
                               static_cast<void>(prefix);
                               static_cast<void>(user_text);
                               throw std::runtime_error("boom");
                           });

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "hello");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_EQ(result.status().message(), "openai llms response building failed");
}

TEST(OpenAiLlmstest, UsesInjectedOpenAiResponsesClientWhenConfigured) {
    auto client = std::make_shared<FakeOpenAiResponsesClient>(absl::OkStatus(), "hello world");
    OpenAiLLMs openai_llms("main", "system prompt", "gpt-5.2", client);

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "hi");

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(client->last_request.model, "gpt-5.2");
    ASSERT_EQ(client->last_request.system_prompt, "system prompt");
    ASSERT_EQ(client->last_request.user_text, "hi");
    EXPECT_EQ(std::get<LlmCallResult>(*result).output_text, "hello world");
}

TEST(OpenAiLlmstest, PropagatesInjectedOpenAiResponsesClientFailure) {
    auto client =
        std::make_shared<FakeOpenAiResponsesClient>(absl::UnavailableError("rate limited"));
    OpenAiLLMs openai_llms("main", "", "gpt-5.2", client);

    const absl::StatusOr<ExecutionStepResult> result = openai_llms.GenerateContent(0, "hi");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(result.status().message(), "rate limited");
}

} // namespace
} // namespace isla::server::ai_gateway
