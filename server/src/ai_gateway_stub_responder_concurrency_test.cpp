#include "ai_gateway_stub_responder_test_support.hpp"

#include <future>
#include <thread>

namespace isla::server::ai_gateway {
namespace {

using namespace test_support;

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
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

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
        .openai_client = test::MakeFakeOpenAiResponsesClient(
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
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

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

TEST(GatewayStubResponderStandaloneTest, MatchingCancelForInProgressTurnEmitsCancelled) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
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
                absl::Status delta_status =
                    on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
                if (!delta_status.ok()) {
                    return delta_status;
                }
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_test",
                });
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

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
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

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
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_one" });
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_two" });

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
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

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
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_one" });
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_two" });

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

} // namespace
} // namespace isla::server::ai_gateway
