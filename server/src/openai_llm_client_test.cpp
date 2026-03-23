#include "isla/server/openai_llm_client.hpp"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/openai_reasoning_effort.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server {
namespace {

using isla::server::ai_gateway::OpenAiReasoningEffort;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesFunctionCallOutputInputItem;
using isla::server::ai_gateway::OpenAiResponsesOutputItem;
using isla::server::ai_gateway::OpenAiResponsesRawInputItem;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
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
    const ai_gateway::test::OpenAiResponsesRequestSnapshot last_request =
        responses_client->last_request_snapshot();
    ASSERT_EQ(last_request.model, "gpt-5.4-mini");
    ASSERT_EQ(last_request.system_prompt, "system");
    ASSERT_EQ(last_request.user_text, "user");
    EXPECT_EQ(last_request.reasoning_effort, OpenAiReasoningEffort::kXHigh);
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

TEST(OpenAiLlmClientTest, RunToolCallRoundTranslatesToolsAndExtractsFunctionCalls) {
    auto responses_client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "ignored", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const ai_gateway::OpenAiResponsesEventCallback& on_event) -> absl::Status {
            EXPECT_TRUE(request.input_items.empty());
            EXPECT_FALSE(request.parallel_tool_calls);
            if (request.function_tools.size() != 2U) {
                ADD_FAILURE() << "expected two translated function tools";
                return absl::FailedPreconditionError("unexpected function tool count");
            }
            EXPECT_EQ(request.function_tools[0].name, "lookup_weather");
            EXPECT_EQ(request.function_tools[0].description, "Look up the weather.");
            EXPECT_EQ(request.function_tools[0].parameters_json_schema,
                      R"({"type":"object","properties":{"city":{"type":"string"}}})");
            EXPECT_TRUE(request.function_tools[0].strict);
            EXPECT_EQ(request.function_tools[1].name, "read_calendar");
            EXPECT_EQ(request.function_tools[1].description, "Read the next calendar event.");
            EXPECT_EQ(request.function_tools[1].parameters_json_schema,
                      R"({"type":"object","properties":{}})");
            EXPECT_FALSE(request.function_tools[1].strict);

            const absl::Status first_status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = "thinking ",
            });
            if (!first_status.ok()) {
                return first_status;
            }
            const absl::Status second_status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = "about tools",
            });
            if (!second_status.ok()) {
                return second_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_tool_round",
                .output_items =
                    {
                        OpenAiResponsesOutputItem{
                            .type = "reasoning",
                            .raw_json = R"({"type":"reasoning","id":"rs_1"})",
                        },
                        OpenAiResponsesOutputItem{
                            .type = "function_call",
                            .raw_json = R"({"type":"function_call","call_id":"call_1"})",
                            .call_id = "call_1",
                            .name = "lookup_weather",
                            .arguments_json = R"({"city":"San Francisco"})",
                        },
                    },
            });
        });
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    const std::vector<LlmFunctionTool> function_tools = {
        LlmFunctionTool{
            .name = "lookup_weather",
            .description = "Look up the weather.",
            .parameters_json_schema =
                R"({"type":"object","properties":{"city":{"type":"string"}}})",
            .strict = true,
        },
        LlmFunctionTool{
            .name = "read_calendar",
            .description = "Read the next calendar event.",
            .parameters_json_schema = R"({"type":"object","properties":{}})",
            .strict = false,
        },
    };

    const absl::StatusOr<LlmToolCallResponse> response =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
            .reasoning_effort = LlmReasoningEffort::kHigh,
        });

    ASSERT_TRUE(response.ok()) << response.status();
    const ai_gateway::test::OpenAiResponsesRequestSnapshot last_request =
        responses_client->last_request_snapshot();
    EXPECT_EQ(last_request.model, "gpt-5.4-mini");
    EXPECT_EQ(last_request.system_prompt, "system");
    EXPECT_EQ(last_request.user_text, "user");
    EXPECT_EQ(last_request.reasoning_effort, OpenAiReasoningEffort::kHigh);
    EXPECT_FALSE(last_request.parallel_tool_calls);
    ASSERT_EQ(response->output_text, "thinking about tools");
    ASSERT_EQ(response->response_id, "resp_tool_round");
    ASSERT_EQ(response->tool_calls.size(), 1U);
    EXPECT_EQ(response->tool_calls[0].call_id, "call_1");
    EXPECT_EQ(response->tool_calls[0].name, "lookup_weather");
    EXPECT_EQ(response->tool_calls[0].arguments_json, R"({"city":"San Francisco"})");
    EXPECT_FALSE(response->continuation_token.empty());
}

TEST(OpenAiLlmClientTest, RunToolCallRoundReplaysContinuationTokenAcrossRounds) {
    constexpr char kReasoningRawJson[] = R"({"type":"reasoning","id":"rs_1"})";
    constexpr char kFunctionCallRawJson[] = R"({"type":"function_call","call_id":"call_1"})";

    int round = 0;
    auto responses_client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "ignored", absl::OkStatus(),
        [&round](const OpenAiResponsesRequest& request,
                 const ai_gateway::OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (round == 0) {
                ++round;
                EXPECT_TRUE(request.input_items.empty());
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_round_1",
                    .output_items =
                        {
                            OpenAiResponsesOutputItem{
                                .type = "reasoning",
                                .raw_json = kReasoningRawJson,
                            },
                            OpenAiResponsesOutputItem{
                                .type = "function_call",
                                .raw_json = kFunctionCallRawJson,
                                .call_id = "call_1",
                                .name = "lookup_weather",
                                .arguments_json = R"({"city":"San Francisco"})",
                            },
                        },
                });
            }

            ++round;
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_round_2",
                .output_items =
                    {
                        OpenAiResponsesOutputItem{
                            .type = "message",
                            .raw_json = R"({"type":"message","role":"assistant","content":"done"})",
                        },
                    },
            });
        });
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    const std::vector<LlmFunctionTool> function_tools = {
        LlmFunctionTool{
            .name = "lookup_weather",
            .description = "Look up the weather.",
            .parameters_json_schema =
                R"({"type":"object","properties":{"city":{"type":"string"}}})",
            .strict = true,
        },
    };

    const absl::StatusOr<LlmToolCallResponse> first_round =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
        });
    ASSERT_TRUE(first_round.ok()) << first_round.status();
    ASSERT_EQ(first_round->tool_calls.size(), 1U);
    EXPECT_EQ(first_round->tool_calls[0].call_id, "call_1");
    EXPECT_FALSE(first_round->continuation_token.empty());

    const std::vector<LlmFunctionCallOutput> tool_outputs = {
        LlmFunctionCallOutput{
            .call_id = "call_1",
            .output = R"({"forecast":"sunny"})",
        },
    };
    const absl::StatusOr<LlmToolCallResponse> second_round =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
            .tool_outputs = std::span<const LlmFunctionCallOutput>(tool_outputs),
            .continuation_token = first_round->continuation_token,
        });
    ASSERT_TRUE(second_round.ok()) << second_round.status();
    EXPECT_TRUE(second_round->tool_calls.empty());

    const std::vector<ai_gateway::test::OpenAiResponsesRequestSnapshot> requests =
        responses_client->requests_snapshot();
    ASSERT_EQ(requests.size(), 2U);
    ASSERT_EQ(requests[1].input_items.size(), 3U);

    const auto* reasoning_item =
        std::get_if<OpenAiResponsesRawInputItem>(&requests[1].input_items[0]);
    ASSERT_TRUE(reasoning_item != nullptr);
    EXPECT_EQ(reasoning_item->raw_json, kReasoningRawJson);

    const auto* function_call_item =
        std::get_if<OpenAiResponsesRawInputItem>(&requests[1].input_items[1]);
    ASSERT_TRUE(function_call_item != nullptr);
    EXPECT_EQ(function_call_item->raw_json, kFunctionCallRawJson);

    const auto* tool_output_item =
        std::get_if<OpenAiResponsesFunctionCallOutputInputItem>(&requests[1].input_items[2]);
    ASSERT_TRUE(tool_output_item != nullptr);
    EXPECT_EQ(tool_output_item->call_id, "call_1");
    EXPECT_EQ(tool_output_item->output, R"({"forecast":"sunny"})");
}

TEST(OpenAiLlmClientTest, RunToolCallRoundRejectsOversizedAggregatedOutput) {
    auto responses_client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "ignored", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const ai_gateway::OpenAiResponsesEventCallback& on_event) -> absl::Status {
            static_cast<void>(request);
            return on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = std::string(ai_gateway::kMaxTextOutputBytes + 1U, 'x'),
            });
        });
    ASSERT_TRUE(responses_client != nullptr);
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOpenAiLlmClient(responses_client);
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<LlmToolCallResponse> response =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "gpt-5.4-mini",
            .system_prompt = "system",
            .user_text = "user",
        });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
}

} // namespace
} // namespace isla::server
