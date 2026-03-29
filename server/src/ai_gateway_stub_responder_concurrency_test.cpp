#include "ai_gateway_stub_responder_test_support.hpp"

#include <future>
#include <thread>

namespace isla::server::ai_gateway {
namespace {

using namespace test_support;

std::shared_ptr<test::FakeOpenAiResponsesClient>
MakeBlockingRequestUserTextEchoClient(const std::shared_ptr<std::promise<void>>& builder_started,
                                      std::shared_future<void> allow_finish_future) {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [builder_started,
         allow_finish_future](const OpenAiResponsesRequest& request,
                              const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (IsMidTermMemoryRequest(request)) {
                return EmitMidTermAwareEchoResponse(request, on_event);
            }
            builder_started->set_value();
            allow_finish_future.wait();
            const std::string text = std::string("stub echo: ") + request.user_text;
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
            if (!delta_status.ok()) {
                return delta_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_test",
            });
        });
}

std::shared_ptr<test::FakeOpenAiResponsesClient>
MakeSelectiveBlockingEchoClient(std::string blocked_latest_line,
                                const std::shared_ptr<std::promise<void>>& blocked_started,
                                std::shared_future<void> allow_finish_future) {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [blocked_latest_line = std::move(blocked_latest_line), blocked_started,
         allow_finish_future](const OpenAiResponsesRequest& request,
                              const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (IsMidTermMemoryRequest(request)) {
                return EmitMidTermAwareEchoResponse(request, on_event);
            }
            const std::string latest_line = test::ExtractLatestPromptLine(request.user_text);
            if (latest_line == blocked_latest_line) {
                blocked_started->set_value();
                allow_finish_future.wait();
            }
            return EmitResponseText(std::string("stub echo: ") + latest_line, on_event);
        });
}

class SelectivelyBlockingSessionStartMemoryStore final : public isla::server::memory::MemoryStore {
  public:
    SelectivelyBlockingSessionStartMemoryStore(std::string blocked_session_id,
                                               std::shared_ptr<std::promise<void>> blocked_started,
                                               std::shared_future<void> allow_finish_future)
        : blocked_session_id_(std::move(blocked_session_id)),
          blocked_started_(std::move(blocked_started)),
          allow_finish_future_(std::move(allow_finish_future)) {}

    absl::Status UpsertSession(const isla::server::memory::MemorySessionRecord& record) override {
        if (record.session_id == blocked_session_id_) {
            blocked_started_->set_value();
            allow_finish_future_.wait();
        }
        return absl::OkStatus();
    }

    absl::Status
    UpsertUserWorkingMemory(const isla::server::memory::UserWorkingMemoryRecord& record) override {
        static_cast<void>(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(
        const isla::server::memory::ConversationMessageWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status ClearSessionWorkingSet(std::string_view session_id) override {
        static_cast<void>(session_id);
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<isla::server::memory::Episode>>
    ListMidTermEpisodes(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::vector<isla::server::memory::Episode>{};
    }

    absl::StatusOr<std::optional<isla::server::memory::Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const override {
        static_cast<void>(session_id);
        static_cast<void>(episode_id);
        return std::nullopt;
    }

    absl::StatusOr<std::optional<isla::server::memory::MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::nullopt;
    }

  private:
    std::string blocked_session_id_;
    std::shared_ptr<std::promise<void>> blocked_started_;
    std::shared_future<void> allow_finish_future_;
};

TEST(GatewayStubResponderStandaloneTest,
     DirectAcceptedTurnCancelWhileProviderBlockedReturnsCancelled) {
    auto request_started = std::make_shared<std::promise<void>>();
    std::shared_future<void> request_started_future = request_started->get_future().share();
    auto allow_response = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_response_future = allow_response->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
            absl::OkStatus(), "", "resp_test", absl::OkStatus(),
            [request_started, allow_response_future](const OpenAiResponsesRequest& request,
                                                     const OpenAiResponsesEventCallback& on_event) {
                if (IsMidTermMemoryRequest(request)) {
                    return EmitMidTermAwareEchoResponse(request, on_event);
                }
                request_started->set_value();
                allow_response_future.wait();
                return EmitResponseText("stub echo: hello", on_event);
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_test",
                                                                  .user_id = "test_user" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    auto turn_future = std::async(std::launch::async, [&] {
        return responder.RunAcceptedTurnToCompletion(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_direct_cancel",
            .text = "hello",
        });
    });

    ASSERT_EQ(request_started_future.wait_for(2s), std::future_status::ready);
    responder.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_direct_cancel",
    });
    allow_response->set_value();

    const absl::StatusOr<GatewayAcceptedTurnResult> result = turn_future.get();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kCancelled);
    EXPECT_FALSE(result->reply_text.has_value());
    EXPECT_FALSE(result->failure.has_value());

    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.cancelled");
    EXPECT_EQ(events[0].turn_id, "turn_direct_cancel");
}

TEST(GatewayStubResponderStandaloneTest, SessionClosedDuringExecutionDropsLaterEmits) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client =
            MakeBlockingRequestUserTextEchoClient(builder_started, allow_finish_future),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_test",
                                                                  .user_id = "test_user" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    session->MarkClosed();
    registry.OnSessionClosed(SessionClosedEvent{
        .session_id = "srv_test",
        .session_started = true,
        .inflight_turn_id = std::string("turn_1"),
        .reason = SessionCloseReason::TransportClosed,
        .detail = "client closed",
    });
    allow_finish->set_value();

    std::this_thread::sleep_for(60ms);
    EXPECT_TRUE(session->events().empty());
}

TEST(GatewayStubResponderStandaloneTest,
     AsyncSessionStartInOtherSessionCompletesWhileFirstSessionStartIsBlocked) {
    auto blocked_started = std::make_shared<std::promise<void>>();
    std::future<void> blocked_started_future = blocked_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();
    auto store = std::make_shared<SelectivelyBlockingSessionStartMemoryStore>(
        "srv_one", blocked_started, allow_finish_future);

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .worker_pool_size = 2,
        .session_start_persistence_max_attempts = 1,
        .session_start_persistence_retry_delay = 0ms,
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });

    std::promise<absl::Status> first_start_promise;
    std::future<absl::Status> first_start_future = first_start_promise.get_future();
    responder.HandleSessionStartAsync(
        SessionStartRequestEvent{ .session_id = "srv_one", .user_id = "user_one" },
        [&first_start_promise](absl::Status status) mutable {
            first_start_promise.set_value(std::move(status));
        });

    ASSERT_EQ(blocked_started_future.wait_for(2s), std::future_status::ready);
    EXPECT_NE(first_start_future.wait_for(100ms), std::future_status::ready);

    std::promise<absl::Status> second_start_promise;
    std::future<absl::Status> second_start_future = second_start_promise.get_future();
    responder.HandleSessionStartAsync(
        SessionStartRequestEvent{ .session_id = "srv_two", .user_id = "user_two" },
        [&second_start_promise](absl::Status status) mutable {
            second_start_promise.set_value(std::move(status));
        });

    ASSERT_EQ(second_start_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(second_start_future.get().ok());
    EXPECT_NE(first_start_future.wait_for(100ms), std::future_status::ready);

    allow_finish->set_value();
    ASSERT_EQ(first_start_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(first_start_future.get().ok());
}

TEST(GatewayStubResponderStandaloneTest, MatchingCancelForInProgressTurnEmitsCancelled) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client =
            MakeBlockingRequestUserTextEchoClient(builder_started, allow_finish_future),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_test",
                                                                  .user_id = "test_user" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    responder.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
    });
    allow_finish->set_value();

    ASSERT_TRUE(session->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.cancelled");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnDuringShutdownDoesNotBlockOnDelayedEmit) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_test",
                                                                  .user_id = "test_user" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    responder.OnServerStopping(registry);
    session->DelayNextErrorCompletion();

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_1",
            .text = "hello",
        });
    });

    EXPECT_EQ(accepted_future.wait_for(100ms), std::future_status::ready);
    ASSERT_TRUE(session->WaitForEventCount(1U));
    {
        const std::vector<EmittedEvent> events = session->events();
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events[0].op, "error");
        EXPECT_EQ(events[0].turn_id, "turn_1");
        EXPECT_EQ(events[0].payload, "server_stopping:server stopping");
    }

    session->ReleasePendingErrorCompletion();
    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest,
     DifferentSessionRenderDoesNotBlockWhileOtherSessionMemoryIsLocked) {
    auto user_query_started = std::make_shared<std::promise<void>>();
    std::future<void> user_query_started_future = user_query_started->get_future();
    auto allow_user_query_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_user_query_finish_future =
        allow_user_query_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_user_query_memory_ready =
            [user_query_started, allow_user_query_finish_future](
                std::string_view session_id,
                const isla::server::memory::UserQueryMemoryResult& user_query_memory_result) {
                static_cast<void>(user_query_memory_result);
                if (session_id != "srv_one") {
                    return;
                }
                user_query_started->set_value();
                allow_user_query_finish_future.wait();
            },
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    registry.RegisterSession(session_one);
    registry.RegisterSession(session_two);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_one",
                                                                  .user_id = "test_user_one" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_one", .user_id = "test_user_one" });
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_two",
                                                                  .user_id = "test_user_two" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_two", .user_id = "test_user_two" });

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_one",
            .turn_id = "turn_1",
            .text = "hello from one",
        });
    });

    ASSERT_EQ(user_query_started_future.wait_for(2s), std::future_status::ready);

    auto render_future = std::async(std::launch::async,
                                    [&] { return responder.RenderSessionMemoryPrompt("srv_two"); });
    ASSERT_EQ(render_future.wait_for(100ms), std::future_status::ready);
    const absl::StatusOr<std::string> prompt = render_future.get();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- (empty)"), std::string::npos);

    allow_user_query_finish->set_value();
    ASSERT_EQ(accepted_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session_one->WaitForEventCount(2U));
}

TEST(GatewayStubResponderStandaloneTest, SameSessionRenderWaitsForOngoingMemoryMutation) {
    auto user_query_started = std::make_shared<std::promise<void>>();
    std::future<void> user_query_started_future = user_query_started->get_future();
    auto allow_user_query_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_user_query_finish_future =
        allow_user_query_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_user_query_memory_ready =
            [user_query_started, allow_user_query_finish_future](
                std::string_view session_id,
                const isla::server::memory::UserQueryMemoryResult& user_query_memory_result) {
                static_cast<void>(user_query_memory_result);
                if (session_id != "srv_test") {
                    return;
                }
                user_query_started->set_value();
                allow_user_query_finish_future.wait();
            },
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_test",
                                                                  .user_id = "test_user" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_1",
            .text = "hello",
        });
    });

    ASSERT_EQ(user_query_started_future.wait_for(2s), std::future_status::ready);

    auto render_future = std::async(
        std::launch::async, [&] { return responder.RenderSessionMemoryPrompt("srv_test"); });
    EXPECT_NE(render_future.wait_for(100ms), std::future_status::ready);

    allow_user_query_finish->set_value();
    ASSERT_EQ(accepted_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(render_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session->WaitForEventCount(2U));
    const absl::StatusOr<std::string> prompt = render_future.get();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- [user | "), std::string::npos);
    EXPECT_NE(prompt->find("] hello"), std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest, ConcurrentMultiSessionTurnsKeepMemoryIsolated) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    registry.RegisterSession(session_one);
    registry.RegisterSession(session_two);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_one",
                                                                  .user_id = "test_user_one" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_one", .user_id = "test_user_one" });
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_two",
                                                                  .user_id = "test_user_two" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_two", .user_id = "test_user_two" });

    auto first_turn = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_one",
            .turn_id = "turn_1",
            .text = "alpha",
        });
    });
    auto second_turn = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_two",
            .turn_id = "turn_2",
            .text = "beta",
        });
    });

    ASSERT_EQ(first_turn.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second_turn.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session_one->WaitForEventCount(2U));
    ASSERT_TRUE(session_two->WaitForEventCount(2U));

    const absl::StatusOr<std::string> prompt_one = responder.RenderSessionMemoryPrompt("srv_one");
    const absl::StatusOr<std::string> prompt_two = responder.RenderSessionMemoryPrompt("srv_two");
    ASSERT_TRUE(prompt_one.ok()) << prompt_one.status();
    ASSERT_TRUE(prompt_two.ok()) << prompt_two.status();
    EXPECT_NE(prompt_one->find("alpha"), std::string::npos);
    EXPECT_NE(prompt_one->find("stub echo: alpha"), std::string::npos);
    EXPECT_EQ(prompt_one->find("beta"), std::string::npos);
    EXPECT_NE(prompt_two->find("beta"), std::string::npos);
    EXPECT_NE(prompt_two->find("stub echo: beta"), std::string::npos);
    EXPECT_EQ(prompt_two->find("alpha"), std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest,
     AcceptedTurnInOtherSessionCompletesWhileFirstSessionExecutionIsBlocked) {
    auto blocked_started = std::make_shared<std::promise<void>>();
    std::future<void> blocked_started_future = blocked_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .worker_pool_size = 2,
        .openai_client =
            MakeSelectiveBlockingEchoClient("alpha", blocked_started, allow_finish_future),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    registry.RegisterSession(session_one);
    registry.RegisterSession(session_two);
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_one",
                                                                  .user_id = "test_user_one" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_one", .user_id = "test_user_one" });
    ASSERT_TRUE(responder
                    .HandleSessionStart(SessionStartRequestEvent{ .session_id = "srv_two",
                                                                  .user_id = "test_user_two" })
                    .ok());
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_two", .user_id = "test_user_two" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_one",
        .turn_id = "turn_1",
        .text = "alpha",
    });

    ASSERT_EQ(blocked_started_future.wait_for(2s), std::future_status::ready);
    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_two",
        .turn_id = "turn_2",
        .text = "beta",
    });

    ASSERT_TRUE(session_two->WaitForEventCount(2U));
    {
        const std::vector<EmittedEvent> session_two_events = session_two->events();
        ASSERT_EQ(session_two_events.size(), 2U);
        EXPECT_EQ(session_two_events[0].op, "text.output");
        EXPECT_EQ(session_two_events[0].turn_id, "turn_2");
        EXPECT_EQ(session_two_events[0].payload, "stub echo: beta");
        EXPECT_EQ(session_two_events[1].op, "turn.completed");
    }
    EXPECT_TRUE(session_one->events().empty());

    allow_finish->set_value();
    ASSERT_TRUE(session_one->WaitForEventCount(2U));
    const std::vector<EmittedEvent> session_one_events = session_one->events();
    ASSERT_EQ(session_one_events.size(), 2U);
    EXPECT_EQ(session_one_events[0].op, "text.output");
    EXPECT_EQ(session_one_events[0].turn_id, "turn_1");
    EXPECT_EQ(session_one_events[0].payload, "stub echo: alpha");
    EXPECT_EQ(session_one_events[1].op, "turn.completed");
}

} // namespace
} // namespace isla::server::ai_gateway
