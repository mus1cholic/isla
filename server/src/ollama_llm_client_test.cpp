#include "isla/server/ollama_llm_client.hpp"

#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_session_handler.hpp"

namespace isla::server {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using nlohmann::json;
using namespace std::chrono_literals;

void ReportTestServerThreadException(std::string_view server_name) {
    try {
        throw;
    } catch (const std::exception& error) {
        ADD_FAILURE() << server_name << " worker thread threw exception: " << error.what();
    } catch (...) {
        ADD_FAILURE() << server_name << " worker thread threw a non-std exception";
    }
}

class ScriptedHttpServer {
  public:
    explicit ScriptedHttpServer(std::vector<std::string> responses)
        : responses_(std::move(responses)), acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~ScriptedHttpServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] bool WaitForRequests(std::size_t expected_count) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (request_texts_.size() >= expected_count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    [[nodiscard]] std::string request_text(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= request_texts_.size()) {
            return "";
        }
        return request_texts_[index];
    }

  private:
    void Stop() {
        stopping_.store(true);
        boost::system::error_code error;
        acceptor_.close(error);
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            for (const std::string& response : responses_) {
                tcp::socket socket(io_context_);
                acceptor_.accept(socket);

                asio::streambuf buffer;
                asio::read_until(socket, buffer, "\r\n\r\n");
                std::string request;
                {
                    std::istream request_stream(&buffer);
                    request.assign(std::istreambuf_iterator<char>(request_stream),
                                   std::istreambuf_iterator<char>());
                }

                const std::size_t header_end = request.find("\r\n\r\n");
                std::size_t content_length = 0U;
                if (header_end != std::string::npos) {
                    const std::string_view headers(request.data(), header_end);
                    constexpr std::string_view kContentLengthHeader = "Content-Length: ";
                    const std::size_t content_length_pos = headers.find(kContentLengthHeader);
                    if (content_length_pos != std::string::npos) {
                        const std::size_t value_begin =
                            content_length_pos + kContentLengthHeader.size();
                        const std::size_t value_end = headers.find("\r\n", value_begin);
                        content_length = static_cast<std::size_t>(std::stoull(
                            std::string(headers.substr(value_begin, value_end - value_begin))));
                    }
                }

                const std::size_t body_already_buffered =
                    header_end == std::string::npos ? 0U : request.size() - (header_end + 4U);
                if (body_already_buffered < content_length) {
                    std::string tail(content_length - body_already_buffered, '\0');
                    asio::read(socket, asio::buffer(tail.data(), tail.size()));
                    request += tail;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    request_texts_.push_back(request);
                }

                asio::write(socket, asio::buffer(response.data(), response.size()));
                boost::system::error_code error;
                socket.shutdown(tcp::socket::shutdown_both, error);
                socket.close(error);
            }
        } catch (...) {
            if (!stopping_.load()) {
                ReportTestServerThreadException("ScriptedHttpServer");
            }
        }
    }

    std::vector<std::string> responses_;
    mutable std::mutex mutex_;
    mutable std::vector<std::string> request_texts_;
    std::atomic<bool> stopping_{ false };
    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

std::string MakeJsonResponse(unsigned int status_code, std::string_view status_text,
                             std::string body) {
    return "HTTP/1.1 " + std::to_string(status_code) + " " + std::string(status_text) + "\r\n" +
           "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

std::string ExtractRequestBody(std::string_view request_text) {
    const std::size_t body_begin = request_text.find("\r\n\r\n");
    if (body_begin == std::string_view::npos) {
        return "";
    }
    return std::string(request_text.substr(body_begin + 4U));
}

TEST(OllamaLlmClientTest, FactoryRejectsDisabledConfig) {
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = false,
        });

    ASSERT_FALSE(client.ok());
    EXPECT_EQ(client.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(OllamaLlmClientTest, StreamsChatResponseAndOmitsAuthorizationWhenUnset) {
    const std::string response_body = R"json({
        "model":"qwen3:4b",
        "created_at":"resp_ollama_1",
        "message":{"role":"assistant","content":"hello from qwen"},
        "done":true
    })json";
    ScriptedHttpServer server({ MakeJsonResponse(200, "OK", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    std::vector<std::string> deltas;
    std::string response_id;
    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "qwen3:4b",
            .system_prompt = "system prompt",
            .user_text = "hello",
            .reasoning_effort = LlmReasoningEffort::kNone,
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
    ASSERT_TRUE(server.WaitForRequests(1U));
    const std::string request_text = server.request_text(0U);
    EXPECT_NE(request_text.find("POST /api/chat HTTP/1.1"), std::string::npos);
    EXPECT_EQ(request_text.find("Authorization: Bearer "), std::string::npos);

    const json request_body = json::parse(ExtractRequestBody(request_text));
    EXPECT_EQ(request_body.at("model"), "qwen3:4b");
    EXPECT_EQ(request_body.at("stream"), false);
    EXPECT_EQ(request_body.at("think"), false);
    ASSERT_EQ(request_body.at("messages").size(), 2U);
    EXPECT_EQ(request_body.at("messages")[0].at("role"), "system");
    EXPECT_EQ(request_body.at("messages")[0].at("content"), "system prompt");
    EXPECT_EQ(request_body.at("messages")[1].at("role"), "user");
    EXPECT_EQ(request_body.at("messages")[1].at("content"), "hello");

    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_EQ(deltas[0], "hello from qwen");
    EXPECT_EQ(response_id, "resp_ollama_1");
}

TEST(OllamaLlmClientTest, IncludesAuthorizationAndEnablesThinkingForReasoningRequests) {
    const std::string response_body = R"json({
        "message":{"role":"assistant","content":"ready"},
        "done":true
    })json";
    ScriptedHttpServer server({ MakeJsonResponse(200, "OK", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
            .api_key = std::string("secret"),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "qwen3:4b",
            .user_text = "hello",
            .reasoning_effort = LlmReasoningEffort::kMedium,
        },
        [](const LlmEvent&) { return absl::OkStatus(); });

    ASSERT_TRUE(status.ok()) << status;
    ASSERT_TRUE(server.WaitForRequests(1U));
    const std::string request_text = server.request_text(0U);
    EXPECT_NE(request_text.find("Authorization: Bearer secret"), std::string::npos);

    const json request_body = json::parse(ExtractRequestBody(request_text));
    EXPECT_EQ(request_body.at("think"), true);
}

TEST(OllamaLlmClientTest, MapsHttpErrorsToAbslStatuses) {
    const std::string response_body = R"json({"error":"model not found"})json";
    ScriptedHttpServer server({ MakeJsonResponse(404, "Not Found", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::Status status = (*client)->StreamResponse(
        LlmRequest{
            .model = "missing-model",
            .user_text = "hello",
        },
        [](const LlmEvent&) { return absl::OkStatus(); });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "model not found");
}

TEST(OllamaLlmClientTest, RunToolCallRoundTranslatesToolsAndExtractsFunctionCalls) {
    const std::string response_body = R"json({
        "created_at":"resp_tool_round",
        "message":{
            "role":"assistant",
            "content":"thinking about tools",
            "tool_calls":[
                {
                    "function":{
                        "name":"lookup_weather",
                        "arguments":{"city":"San Francisco"}
                    }
                },
                {
                    "function":{
                        "name":"read_calendar",
                        "arguments":{}
                    }
                }
            ]
        },
        "done":true
    })json";
    ScriptedHttpServer server({ MakeJsonResponse(200, "OK", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();
    EXPECT_TRUE((*client)->SupportsToolCalling());

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
            .model = "qwen3:4b",
            .system_prompt = "system prompt",
            .user_text = "hello",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
            .reasoning_effort = LlmReasoningEffort::kMedium,
        });

    ASSERT_TRUE(response.ok()) << response.status();
    ASSERT_TRUE(server.WaitForRequests(1U));
    const json request_body = json::parse(ExtractRequestBody(server.request_text(0U)));
    EXPECT_EQ(request_body.at("model"), "qwen3:4b");
    EXPECT_EQ(request_body.at("stream"), false);
    EXPECT_EQ(request_body.at("think"), true);
    ASSERT_EQ(request_body.at("messages").size(), 2U);
    EXPECT_EQ(request_body.at("messages")[0].at("role"), "system");
    EXPECT_EQ(request_body.at("messages")[0].at("content"), "system prompt");
    EXPECT_EQ(request_body.at("messages")[1].at("role"), "user");
    EXPECT_EQ(request_body.at("messages")[1].at("content"), "hello");
    ASSERT_EQ(request_body.at("tools").size(), 2U);
    EXPECT_EQ(request_body.at("tools")[0].at("type"), "function");
    EXPECT_EQ(request_body.at("tools")[0].at("function").at("name"), "lookup_weather");
    EXPECT_EQ(request_body.at("tools")[0].at("function").at("description"), "Look up the weather.");
    EXPECT_EQ(request_body.at("tools")[0].at("function").at("parameters"),
              json::parse(R"({"type":"object","properties":{"city":{"type":"string"}}})"));
    EXPECT_EQ(request_body.at("tools")[1].at("function").at("name"), "read_calendar");
    EXPECT_EQ(request_body.at("tools")[1].at("function").at("parameters"),
              json::parse(R"({"type":"object","properties":{}})"));

    EXPECT_EQ(response->output_text, "thinking about tools");
    EXPECT_EQ(response->response_id, "resp_tool_round");
    ASSERT_EQ(response->tool_calls.size(), 2U);
    EXPECT_EQ(response->tool_calls[0].call_id, "ollama_tool_call_0");
    EXPECT_EQ(response->tool_calls[0].name, "lookup_weather");
    EXPECT_EQ(response->tool_calls[0].arguments_json, R"({"city":"San Francisco"})");
    EXPECT_EQ(response->tool_calls[1].call_id, "ollama_tool_call_1");
    EXPECT_EQ(response->tool_calls[1].name, "read_calendar");
    EXPECT_EQ(response->tool_calls[1].arguments_json, R"({})");
    EXPECT_FALSE(response->continuation_token.empty());
}

TEST(OllamaLlmClientTest, RunToolCallRoundReplaysContinuationTokenAcrossRounds) {
    const std::string first_response_body = R"json({
        "created_at":"resp_round_1",
        "message":{
            "role":"assistant",
            "tool_calls":[
                {
                    "function":{
                        "name":"lookup_weather",
                        "arguments":{"city":"San Francisco"}
                    }
                }
            ]
        },
        "done":true
    })json";
    const std::string second_response_body = R"json({
        "created_at":"resp_round_2",
        "message":{
            "role":"assistant",
            "content":"done"
        },
        "done":true
    })json";
    ScriptedHttpServer server({
        MakeJsonResponse(200, "OK", first_response_body),
        MakeJsonResponse(200, "OK", second_response_body),
    });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
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
            .model = "qwen3:4b",
            .system_prompt = "system prompt",
            .user_text = "hello",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
        });
    ASSERT_TRUE(first_round.ok()) << first_round.status();
    ASSERT_EQ(first_round->tool_calls.size(), 1U);
    EXPECT_EQ(first_round->tool_calls[0].call_id, "ollama_tool_call_0");
    EXPECT_FALSE(first_round->continuation_token.empty());

    const std::vector<LlmFunctionCallOutput> tool_outputs = {
        LlmFunctionCallOutput{
            .call_id = first_round->tool_calls[0].call_id,
            .output = "22C",
        },
    };
    const absl::StatusOr<LlmToolCallResponse> second_round =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "qwen3:4b",
            .system_prompt = "system prompt",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
            .tool_outputs = std::span<const LlmFunctionCallOutput>(tool_outputs),
            .continuation_token = first_round->continuation_token,
        });
    ASSERT_TRUE(second_round.ok()) << second_round.status();
    EXPECT_TRUE(second_round->tool_calls.empty());
    EXPECT_EQ(second_round->output_text, "done");

    ASSERT_TRUE(server.WaitForRequests(2U));
    const json first_request_body = json::parse(ExtractRequestBody(server.request_text(0U)));
    ASSERT_EQ(first_request_body.at("messages").size(), 2U);
    EXPECT_EQ(first_request_body.at("messages")[0].at("role"), "system");
    EXPECT_EQ(first_request_body.at("messages")[0].at("content"), "system prompt");
    EXPECT_EQ(first_request_body.at("messages")[1].at("role"), "user");
    EXPECT_EQ(first_request_body.at("messages")[1].at("content"), "hello");

    const json second_request_body = json::parse(ExtractRequestBody(server.request_text(1U)));
    ASSERT_EQ(second_request_body.at("messages").size(), 4U);
    EXPECT_EQ(second_request_body.at("messages")[0].at("role"), "system");
    EXPECT_EQ(second_request_body.at("messages")[0].at("content"), "system prompt");
    EXPECT_EQ(second_request_body.at("messages")[1].at("role"), "user");
    EXPECT_EQ(second_request_body.at("messages")[1].at("content"), "hello");
    EXPECT_EQ(second_request_body.at("messages")[2].at("role"), "assistant");
    ASSERT_EQ(second_request_body.at("messages")[2].at("tool_calls").size(), 1U);
    EXPECT_EQ(second_request_body.at("messages")[2].at("tool_calls")[0].at("type"), "function");
    EXPECT_EQ(second_request_body.at("messages")[2].at("tool_calls")[0].at("function").at("index"),
              0);
    EXPECT_EQ(second_request_body.at("messages")[2].at("tool_calls")[0].at("function").at("name"),
              "lookup_weather");
    EXPECT_EQ(
        second_request_body.at("messages")[2].at("tool_calls")[0].at("function").at("arguments"),
        json::parse(R"({"city":"San Francisco"})"));
    EXPECT_EQ(second_request_body.at("messages")[3].at("role"), "tool");
    EXPECT_EQ(second_request_body.at("messages")[3].at("tool_name"), "lookup_weather");
    EXPECT_EQ(second_request_body.at("messages")[3].at("content"), "22C");
}

TEST(OllamaLlmClientTest, RunToolCallRoundRejectsOversizedAggregatedOutput) {
    const std::string response_body =
        json{
            { "created_at", "resp_tool_round" },
            { "message",
              {
                  { "role", "assistant" },
                  { "content", std::string(ai_gateway::kMaxTextOutputBytes + 1U, 'x') },
              } },
            { "done", true },
        }
            .dump();
    ScriptedHttpServer server({ MakeJsonResponse(200, "OK", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<LlmToolCallResponse> response =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "qwen3:4b",
            .user_text = "hello",
        });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(OllamaLlmClientTest, RunToolCallRoundRejectsOversizedContinuationToken) {
    const std::string long_city = std::string(256U * 1024U, 'x');
    const std::string response_body =
        json{
            { "created_at", "resp_tool_round" },
            { "message",
              {
                  { "role", "assistant" },
                  { "tool_calls", json::array({
                                      json{
                                          { "function",
                                            {
                                                { "name", "lookup_weather" },
                                                { "arguments", json{ { "city", long_city } } },
                                            } },
                                      },
                                  }) },
              } },
            { "done", true },
        }
            .dump();
    ScriptedHttpServer server({ MakeJsonResponse(200, "OK", response_body) });
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateOllamaLlmClient(OllamaLlmClientConfig{
            .enabled = true,
            .base_url = "http://127.0.0.1:" + std::to_string(server.port()),
        });
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

    const absl::StatusOr<LlmToolCallResponse> response =
        (*client)->RunToolCallRound(LlmToolCallRequest{
            .model = "qwen3:4b",
            .user_text = "hello",
            .function_tools = std::span<const LlmFunctionTool>(function_tools),
        });

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
}

} // namespace
} // namespace isla::server
