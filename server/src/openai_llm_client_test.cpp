#include "isla/server/openai_llm_client.hpp"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "isla/server/llm_client.hpp"
#include "isla/server/openai_reasoning_effort.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server {
namespace {

using isla::server::ai_gateway::OpenAiReasoningEffort;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;

TEST(OpenAiLlmClientTest, FactoryRejectsNullResponsesClient) {
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client = CreateOpenAiLlmClient(nullptr);

    ASSERT_FALSE(client.ok());
    EXPECT_EQ(client.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(OpenAiLlmClientTest, ValidateAndWarmUpDelegateToWrappedClient) {
    auto responses_client = MakeFakeOpenAiResponsesClient(absl::OkStatus(), "", "resp_test",
                                                          absl::UnauthenticatedError("bad key"), {},
                                                          absl::UnavailableError("cold"));
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    EXPECT_EQ((*client)->Validate().code(), absl::StatusCode::kUnauthenticated);
    EXPECT_EQ((*client)->WarmUp().code(), absl::StatusCode::kUnavailable);
}

TEST(OpenAiLlmClientTest, TranslatesRequestsAndEvents) {
    auto responses_client = MakeFakeOpenAiResponsesClient(absl::OkStatus(), "hello world");
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    std::vector<std::string> deltas;
    std::string response_id;
    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
            .reasoning_effort = LlmReasoningEffort::kXHigh,
        },
        [&deltas, &response_id](const LlmEvent& event) -> absl::Status {
            return std::visit(
                [&deltas, &response_id](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, LlmTextDeltaEvent>) {
                        deltas.push_back(concrete_event.text_delta);
                    }
                    if constexpr (std::is_same_v<Event, LlmCompletedEvent>) {
                        response_id = concrete_event.response_id;
                    }
                    return absl::OkStatus();
                },
                event);
        });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_EQ(responses_client->last_request.model, "gpt-5.4-mini");
    ASSERT_EQ(responses_client->last_request.system_prompt, "system");
    ASSERT_EQ(responses_client->last_request.user_text, "user");
    EXPECT_EQ(responses_client->last_request.reasoning_effort, OpenAiReasoningEffort::kXHigh);
    ASSERT_EQ(deltas.size(), 2U);
    EXPECT_EQ(deltas[0] + deltas[1], "hello world");
    EXPECT_EQ(response_id, "resp_test");
}

TEST(OpenAiLlmClientTest, RejectsUnknownReasoningEffort) {
    auto responses_client = MakeFakeOpenAiResponsesClient(absl::OkStatus(), "ignored");
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
            .reasoning_effort = static_cast<LlmReasoningEffort>(999),
        },
        [](const LlmEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server
