#include "server/src/evals/live_eval_runner.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>

#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "server/src/openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using namespace std::chrono_literals;
using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::ai_gateway::SequentialSessionIdGenerator;
using isla::server::ai_gateway::test::ExtractLatestPromptLine;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::ConversationMessageWrite;
using isla::server::memory::Episode;
using isla::server::memory::MemorySessionRecord;
using isla::server::memory::MemoryStore;
using isla::server::memory::MemoryStoreSnapshot;
using isla::server::memory::MessageRole;

absl::Status EmitResponseText(std::string_view text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id = "resp_live_eval") {
    const absl::Status delta_status =
        on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = std::string(text) });
    if (!delta_status.ok()) {
        return delta_status;
    }
    return on_event(OpenAiResponsesCompletedEvent{
        .response_id = std::string(response_id),
    });
}

absl::Status EmitMidTermAwareReply(const OpenAiResponsesRequest& request,
                                   const OpenAiResponsesEventCallback& on_event) {
    if (request.system_prompt.find("should_flush") != std::string::npos) {
        return EmitResponseText(R"json({
            "should_flush": false,
            "item_id": null,
            "split_at": null,
            "reasoning": "No completed episode boundary."
        })json",
                                on_event, "resp_live_eval_decider");
    }
    if (request.system_prompt.find("tier2_summary") != std::string::npos) {
        return EmitResponseText(R"json({
            "tier1_detail": "detail",
            "tier2_summary": "summary",
            "tier3_ref": "ref",
            "tier3_keywords": ["live", "eval", "test", "memory", "summary"],
            "salience": 5
        })json",
                                on_event, "resp_live_eval_compactor");
    }
    return EmitResponseText("stub live reply: " + ExtractLatestPromptLine(request.user_text),
                            on_event);
}

class RecordingMemoryStore final : public MemoryStore {
  public:
    absl::Status UpsertSession(const MemorySessionRecord& record) override {
        session_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(const ConversationMessageWrite& write) override {
        message_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& write) override {
        stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& write) override {
        mid_term_episode_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& write) override {
        split_stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view /*session_id*/) const override {
        return std::vector<Episode>{};
    }

    absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view /*session_id*/,
                      std::string_view /*episode_id*/) const override {
        return std::nullopt;
    }

    absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view /*session_id*/) const override {
        return std::nullopt;
    }

    std::vector<MemorySessionRecord> session_records;
    std::vector<ConversationMessageWrite> message_writes;
    std::vector<isla::server::memory::EpisodeStubWrite> stub_writes;
    std::vector<isla::server::memory::SplitEpisodeStubWrite> split_stub_writes;
    std::vector<isla::server::memory::MidTermEpisodeWrite> mid_term_episode_writes;
};

class ScopedLiveGatewayServer {
  public:
    explicit ScopedLiveGatewayServer(GatewayStubResponderConfig responder_config)
        : responder_(std::move(responder_config)),
          server_(
              GatewayServerConfig{
                  .bind_host = "127.0.0.1",
                  .port = 0,
                  .listen_backlog = 4,
              },
              &responder_, std::make_unique<SequentialSessionIdGenerator>("live_eval_")) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    ~ScopedLiveGatewayServer() {
        Stop();
    }

    [[nodiscard]] absl::Status Start() {
        return server_.Start();
    }

    void Stop() {
        if (stopped_) {
            return;
        }
        server_.Stop();
        stopped_ = true;
    }

    [[nodiscard]] std::uint16_t port() const {
        return server_.bound_port();
    }

  private:
    GatewayStubResponder responder_;
    GatewayServer server_;
    bool stopped_ = false;
};

TEST(LiveEvalRunnerTest, SeedsHistoricalAssistantMessagesThroughLiveGateway) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_live_eval", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request, const OpenAiResponsesEventCallback& on_event)
            -> absl::Status { return EmitMidTermAwareReply(request, on_event); });

    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .memory_store = store,
        .openai_client = client,
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const LiveEvalRunner runner(LiveEvalRunnerConfig{
        .host = "127.0.0.1",
        .port = live_gateway.port(),
        .path = "/",
        .operation_timeout = 2s,
        .turn_completion_timeout = 2s,
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "live_eval_test",
        .case_id = "assistant_seeded_history",
        .session_id = "seeded_history_session",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::User,
                    .text = "hello from setup",
                },
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "seeded assistant context",
                },
            },
        .input =
            EvalInput{
                .text = "what did i just say?",
            },
    });

    ASSERT_TRUE(artifacts.ok()) << artifacts.status();
    EXPECT_EQ(artifacts->status, EvalTurnStatus::kSucceeded);
    ASSERT_TRUE(artifacts->final_reply.has_value());
    EXPECT_EQ(*artifacts->final_reply, "stub live reply: what did i just say?");

    ASSERT_EQ(store->session_records.size(), 1U);
    ASSERT_GE(store->message_writes.size(), 4U);
    EXPECT_EQ(store->message_writes[0].turn_id, "history_turn_1");
    EXPECT_EQ(store->message_writes[0].role, MessageRole::User);
    EXPECT_EQ(store->message_writes[0].content, "hello from setup");
    EXPECT_EQ(store->message_writes[1].turn_id, "history_turn_1");
    EXPECT_EQ(store->message_writes[1].role, MessageRole::Assistant);
    EXPECT_EQ(store->message_writes[1].content, "seeded assistant context");
    EXPECT_EQ(store->message_writes[2].turn_id, "evaluated_turn");
    EXPECT_EQ(store->message_writes[2].role, MessageRole::User);

    ASSERT_EQ(artifacts->replayed_session_history.size(), 5U);
    EXPECT_EQ(artifacts->replayed_session_history[0].kind, EvalReplayEventKind::kSessionStart);
    EXPECT_EQ(artifacts->replayed_session_history[1].turn_id,
              std::optional<std::string>("history_turn_1"));
    EXPECT_EQ(artifacts->replayed_session_history[1].role, std::optional<std::string>("user"));
    EXPECT_EQ(artifacts->replayed_session_history[2].turn_id,
              std::optional<std::string>("history_turn_1"));
    EXPECT_EQ(artifacts->replayed_session_history[2].role, std::optional<std::string>("assistant"));
    EXPECT_EQ(artifacts->replayed_session_history[2].text,
              std::optional<std::string>("seeded assistant context"));
}

TEST(LiveEvalRunnerTest, RejectsEmptyAssistantHistoryMessageBeforeConnecting) {
    const LiveEvalRunner runner(LiveEvalRunnerConfig{
        .host = "127.0.0.1",
        .port = 12345,
        .path = "/",
        .operation_timeout = 2s,
        .turn_completion_timeout = 2s,
    });

    const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(EvalCase{
        .benchmark_name = "live_eval_test",
        .case_id = "empty_assistant_history",
        .session_id = "empty_assistant_history_session",
        .conversation =
            {
                EvalConversationMessage{
                    .role = MessageRole::Assistant,
                    .text = "",
                },
            },
        .input =
            EvalInput{
                .text = "what did i just say?",
            },
    });

    ASSERT_FALSE(artifacts.ok());
    EXPECT_EQ(artifacts.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(artifacts.status().message(),
              "live eval case conversation assistant text must not be empty");
}

} // namespace
} // namespace isla::server::evals
