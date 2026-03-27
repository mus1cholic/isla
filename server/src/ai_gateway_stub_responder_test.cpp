#include "ai_gateway_stub_responder_test_support.hpp"

#include <array>

namespace isla::server::ai_gateway {
namespace {

using namespace test_support;

struct PromptBudgetCase {
    std::string name;
    std::function<void(GatewayStubResponderConfig&)> configure;
    std::string expected_second_turn_op;
    std::string expected_second_turn_payload;
    bool expected_payload_is_prefix = false;
};

class GatewayStubResponderPromptBudgetTest
    : public GatewayStubResponderStandaloneFixture,
      public ::testing::WithParamInterface<PromptBudgetCase> {};

TEST_F(GatewayStubResponderTest, SessionStartCreatesMemoryPromptBeforeAnyTurn) {
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("<conversation>"), std::string::npos);
    EXPECT_NE(prompt->find("- (empty)"), std::string::npos);
}

TEST_F(GatewayStubResponderStandaloneFixture, SessionStartPersistsSessionBeforeFirstTurn) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    InitializeResponder(MakeStoreEchoConfig(store));

    StartSession();
    ASSERT_EQ(store->session_records.size(), 1U);

    AcceptTurn("turn_1", "hello");

    ASSERT_TRUE(session().WaitForEventCount(2U));
    EXPECT_EQ(store->session_records.size(), 1U);
    ASSERT_EQ(store->message_writes.size(), 2U);
    EXPECT_EQ(store->message_writes[0].content, "hello");
    EXPECT_EQ(store->message_writes[1].content, "stub echo: hello");
}

TEST_F(GatewayStubResponderStandaloneFixture, SessionStartRetriesTransientPersistenceFailures) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::UnavailableError("supabase unavailable"),
        absl::DeadlineExceededError("supabase timeout"),
    };
    GatewayStubResponderConfig config = MakeStoreEchoConfig(store);
    config.session_start_persistence_max_attempts = 3;
    config.session_start_persistence_retry_delay = 0ms;
    InitializeResponder(std::move(config));

    StartSession();

    EXPECT_EQ(store->upsert_session_attempts, 3U);
    ASSERT_EQ(store->session_records.size(), 1U);
    EXPECT_TRUE(session().events().empty());
}

TEST_F(GatewayStubResponderStandaloneFixture, SessionStartDoesNotRetryNonRetryableFailures) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::PermissionDeniedError("supabase denied"),
        absl::UnavailableError("should not be consumed"),
    };
    GatewayStubResponderConfig config = MakeStoreEchoConfig(store);
    config.session_start_persistence_max_attempts = 3;
    config.session_start_persistence_retry_delay = 0ms;
    InitializeResponder(std::move(config));

    StartSession();

    const std::vector<EmittedEvent> events = WaitForEvents(1U);
    EXPECT_EQ(store->upsert_session_attempts, 1U);
    ASSERT_TRUE(store->session_records.empty());
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "");
    EXPECT_EQ(events[0].payload, "internal_error:failed to initialize session memory");
}

TEST_F(GatewayStubResponderStandaloneFixture, DuplicateSessionStartDoesNotPoisonHealthySession) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    InitializeResponder(MakeStoreEchoConfig(store));

    StartSession();
    StartSession();

    {
        const std::vector<EmittedEvent> startup_events = WaitForEvents(1U);
        ASSERT_EQ(startup_events.size(), 1U);
        EXPECT_EQ(startup_events[0].op, "error");
        EXPECT_EQ(startup_events[0].payload, "internal_error:failed to initialize session memory");
    }

    AcceptTurn("turn_1", "hello");

    const std::vector<EmittedEvent> events = WaitForEvents(3U);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1].op, "text.output");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[1].payload, "stub echo: hello");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderStandaloneFixture,
       SessionStartExhaustedRetriesEmitsErrorAndDoesNotRetryOnFirstTurn) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::UnavailableError("supabase unavailable"),
        absl::UnavailableError("supabase still unavailable"),
        absl::UnavailableError("supabase unavailable again"),
    };
    GatewayStubResponderConfig config = MakeStoreEchoConfig(store);
    config.session_start_persistence_max_attempts = 3;
    config.session_start_persistence_retry_delay = 0ms;
    InitializeResponder(std::move(config));

    StartSession();
    ASSERT_TRUE(session().WaitForEventCount(1U));
    EXPECT_EQ(store->upsert_session_attempts, 3U);
    ASSERT_EQ(store->session_records.size(), 0U);

    AcceptTurn("turn_1", "hello");

    const std::vector<EmittedEvent> events = WaitForEvents(3U);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "");
    EXPECT_EQ(events[0].payload, "service_unavailable:failed to initialize session memory");
    EXPECT_EQ(events[1].op, "error");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[1].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_1");
    EXPECT_EQ(store->upsert_session_attempts, 3U);
}

TEST_F(GatewayStubResponderTest, AcceptedTurnProvidesRenderedPromptPiecesToOpenAiClient) {
    auto capturing_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "reply", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (IsMidTermMemoryRequest(request)) {
                return EmitMidTermAwareEchoResponse(request, on_event);
            }
            return EmitResponseText("reply", on_event);
        });

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = capturing_client,
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });
    const std::shared_ptr<const TurnTelemetryContext> telemetry_context =
        MakeTurnTelemetryContext("srv_test", "turn_1");

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
        .telemetry_context = telemetry_context,
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<test::OpenAiResponsesRequestSnapshot> requests =
        capturing_client->requests_snapshot();
    auto main_request = std::find_if(requests.rbegin(), requests.rend(), [](const auto& request) {
        return !IsMidTermMemoryRequest(OpenAiResponsesRequest{
            .system_prompt = request.system_prompt,
        });
    });
    ASSERT_NE(main_request, requests.rend());
    EXPECT_NE(main_request->system_prompt.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("- [user | "), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("- [assistant | "), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("] hello"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<conversation>"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<mid_term_episodes>"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<retrieved_memory>"), std::string::npos);
    EXPECT_EQ(main_request->user_text.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_EQ(main_request->user_text.find("You are Isla."), std::string::npos);
    EXPECT_NE(main_request->user_text.find("] hello"), std::string::npos);
    EXPECT_EQ(main_request->telemetry_context, telemetry_context);
}

TEST(GatewayStubResponderStandaloneTest,
     ExtractLatestPromptLinePreservesBracketSpaceInsideConversationContent) {
    EXPECT_EQ(test::ExtractLatestPromptLine(
                  "<mid_term_episodes>\n- (none)\n<retrieved_memory>\n(none)\n<conversation>\n"
                  "- [user | 2026-03-14T20:00:00.000Z] hello ] world\n"),
              "hello ] world");
}

TEST_F(GatewayStubResponderTest,
       SecondTurnRequestIncludesPriorConversationWithoutDuplicatingCurrentTurn) {
    auto capturing_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request, const OpenAiResponsesEventCallback& on_event)
            -> absl::Status { return EmitMidTermAwareEchoResponse(request, on_event); });

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = capturing_client,
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(
        SessionStartedEvent{ .session_id = "srv_test", .user_id = "test_user" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    ASSERT_TRUE(session->WaitForEventCount(2U));

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "how are you?",
    });
    ASSERT_TRUE(session->WaitForEventCount(4U));

    const std::vector<test::OpenAiResponsesRequestSnapshot> requests =
        capturing_client->requests_snapshot();
    auto second_request = std::find_if(requests.rbegin(), requests.rend(), [](const auto& request) {
        return !IsMidTermMemoryRequest(OpenAiResponsesRequest{
            .system_prompt = request.system_prompt,
        });
    });
    ASSERT_NE(second_request, requests.rend());
    const std::string& second_request_context = second_request->user_text;
    EXPECT_NE(second_request_context.find("<conversation>"), std::string::npos);
    EXPECT_NE(second_request_context.find("] hello"), std::string::npos);
    EXPECT_NE(second_request_context.find("] stub echo: hello"), std::string::npos);
    EXPECT_NE(second_request_context.find("] how are you?"), std::string::npos);

    const std::size_t current_turn_pos = second_request_context.find("] how are you?");
    ASSERT_NE(current_turn_pos, std::string::npos);
    EXPECT_EQ(second_request_context.find("] how are you?", current_turn_pos + 1U),
              std::string::npos);
}

TEST_F(GatewayStubResponderTest, SessionStartUsesBundledSystemPromptByDefault) {
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    const absl::StatusOr<std::string> system_prompt = isla::server::memory::LoadSystemPrompt();

    ASSERT_TRUE(prompt.ok()) << prompt.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_EQ(prompt->compare(0, system_prompt->size(), *system_prompt), 0);
}

TEST_F(GatewayStubResponderTest, AcceptedTurnEmitsStubTextAndCompletion) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest,
       RunAcceptedTurnToCompletionReturnsSucceededAndEmitsTerminalEvents) {
    const absl::StatusOr<GatewayAcceptedTurnResult> result =
        responder_.RunAcceptedTurnToCompletion(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_direct_1",
            .text = "hello",
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kSucceeded);
    ASSERT_TRUE(result->reply_text.has_value());
    EXPECT_EQ(*result->reply_text, "stub echo: hello");
    EXPECT_FALSE(result->failure.has_value());

    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_direct_1");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_direct_1");
}

TEST_F(GatewayStubResponderTest, RunAcceptedTurnToCompletionUpdatesSessionMemoryPromptAndSnapshot) {
    const absl::StatusOr<GatewayAcceptedTurnResult> result =
        responder_.RunAcceptedTurnToCompletion(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_direct_memory",
            .text = "hello",
        });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kSucceeded);

    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("] hello"), std::string::npos);
    EXPECT_NE(prompt->find("] stub echo: hello"), std::string::npos);

    const absl::StatusOr<isla::server::memory::WorkingMemoryState> state =
        responder_.SnapshotSessionWorkingMemoryState("srv_test");
    ASSERT_TRUE(state.ok()) << state.status();
    ASSERT_EQ(state->conversation.items.size(), 1U);
    ASSERT_TRUE(state->conversation.items.front().ongoing_episode.has_value());
    const auto& messages = state->conversation.items.front().ongoing_episode->messages;
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].content, "hello");
    EXPECT_EQ(messages[1].content, "stub echo: hello");
}

TEST_F(GatewayStubResponderTest, SuccessfulTurnEmitsPhaseTwoTelemetrySlices) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_success",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_success", telemetry_sink),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventMemoryUserQueryStarted,
        telemetry::kEventMemoryUserQueryCompleted,
        telemetry::kEventTurnEnqueued,
        telemetry::kEventTurnDequeued,
        telemetry::kEventPlanCreateStarted,
        telemetry::kEventPlanCreateCompleted,
        telemetry::kEventExecutorStarted,
        telemetry::kEventExecutorCompleted,
        telemetry::kEventTextOutputEmitStarted,
        telemetry::kEventTextOutputEmitCompleted,
        telemetry::kEventMemoryAssistantReplyStarted,
        telemetry::kEventMemoryAssistantReplyCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }

    const std::array expected_phase_names = {
        telemetry::kPhaseMemoryUserQuery,   telemetry::kPhaseQueueWait,
        telemetry::kPhasePlanCreate,        telemetry::kPhaseExecutorTotal,
        telemetry::kPhaseEmitTextOutput,    telemetry::kPhaseMemoryAssistantReply,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    for (const TelemetryPhaseRecord& phase : phases) {
        EXPECT_LE(phase.started_at, phase.completed_at);
    }

    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeSucceeded);
}

TEST_F(GatewayStubResponderStandaloneFixture, AcceptedTurnFlowsThroughPlannerAndExecutorBoundary) {
    std::optional<ExecutionPlan> execution_plan;

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.memory_user_id = "gateway_user";
    config.on_execution_plan = [&](const ExecutionPlan& plan) { execution_plan = plan; };
    InitializeResponder(std::move(config));
    StartSession();

    AcceptTurn("turn_1", "hello");

    ASSERT_TRUE(session().WaitForEventCount(2U));
    ASSERT_TRUE(execution_plan.has_value());
    ASSERT_EQ(execution_plan->steps.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<OpenAiLlmStep>(execution_plan->steps.front()));
    const OpenAiLlmStep& openai_step = std::get<OpenAiLlmStep>(execution_plan->steps.front());
    EXPECT_EQ(openai_step.step_name, "main");
    EXPECT_EQ(openai_step.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(openai_step.reasoning_effort, OpenAiReasoningEffort::kMedium);

    const std::vector<EmittedEvent> events = session().events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
}

TEST_F(GatewayStubResponderStandaloneFixture, MissingSessionMemoryStillEmitsFailureTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.memory_user_id = "gateway_user";
    InitializeResponder(std::move(config));

    responder().OnTurnAccepted(TurnAcceptedEvent{
        .session_id = session_id(),
        .turn_id = "turn_missing_memory",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_missing_memory", telemetry_sink),
    });

    ASSERT_TRUE(session().WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<EmittedEvent> emitted = session().events();
    ASSERT_EQ(emitted.size(), 2U);
    EXPECT_EQ(emitted[0].op, "error");
    EXPECT_EQ(emitted[0].turn_id, "turn_missing_memory");
    EXPECT_EQ(emitted[1].op, "turn.completed");

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventMemoryUserQueryStarted,
        telemetry::kEventMemoryUserQueryCompleted,
        telemetry::kEventTurnFailed,
        telemetry::kEventErrorEmitStarted,
        telemetry::kEventErrorEmitCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseMemoryUserQuery,
        telemetry::kPhaseEmitError,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeFailed);
}

TEST_F(GatewayStubResponderStandaloneFixture,
       DirectAcceptedTurnFailureReturnsFailedOutcomeAndEmitsTerminalEvents) {
    GatewayStubResponderConfig config = MakeEchoConfig();
    config.memory_user_id = "gateway_user";
    InitializeResponder(std::move(config));

    const absl::StatusOr<GatewayAcceptedTurnResult> result =
        RunAcceptedTurnToCompletion("turn_direct_missing_memory", "hello");

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kFailed);
    ASSERT_TRUE(result->failure.has_value());
    EXPECT_EQ(result->failure->code, "internal_error");
    EXPECT_EQ(result->failure->message, "stub responder failed to update memory");
    EXPECT_FALSE(result->reply_text.has_value());

    const std::vector<EmittedEvent> emitted = session().events();
    ASSERT_EQ(emitted.size(), 2U);
    EXPECT_EQ(emitted[0].op, "error");
    EXPECT_EQ(emitted[0].turn_id, "turn_direct_missing_memory");
    EXPECT_EQ(emitted[0].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(emitted[1].op, "turn.completed");
    EXPECT_EQ(emitted[1].turn_id, "turn_direct_missing_memory");
}

TEST_F(GatewayStubResponderStandaloneFixture,
       DirectAcceptedTurnPreservesReplyWhenAssistantMemoryWritebackFails) {
    InitializeResponder(MakeEchoConfig());
    StartSession();
    session().SetEventHook([this](const EmittedEvent& event) {
        if (event.op == "text.output" && event.turn_id == "turn_direct_post_emit_memory_failure") {
            responder().OnSessionClosed(SessionClosedEvent{
                .session_id = session_id(),
                .session_started = true,
                .inflight_turn_id = std::nullopt,
                .reason = SessionCloseReason::ProtocolEnded,
                .detail = "session ended",
            });
        }
    });

    const absl::StatusOr<GatewayAcceptedTurnResult> result =
        RunAcceptedTurnToCompletion("turn_direct_post_emit_memory_failure", "hello");

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kFailed);
    ASSERT_TRUE(result->failure.has_value());
    EXPECT_EQ(result->failure->code, "internal_error");
    EXPECT_EQ(result->failure->message, "stub responder failed to update memory");
    ASSERT_TRUE(result->reply_text.has_value());
    EXPECT_EQ(*result->reply_text, "stub echo: hello");

    const std::vector<EmittedEvent> events = session().events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_direct_post_emit_memory_failure");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "error");
    EXPECT_EQ(events[1].turn_id, "turn_direct_post_emit_memory_failure");
    EXPECT_EQ(events[1].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_direct_post_emit_memory_failure");
}

TEST_F(GatewayStubResponderStandaloneFixture,
       DirectAcceptedTurnAfterServerStoppingReturnsServerStoppingFailure) {
    InitializeResponder(MakeEchoConfig());
    StartSession();

    registry_attachment_->registry().NotifyServerStopping();

    const absl::StatusOr<GatewayAcceptedTurnResult> result =
        RunAcceptedTurnToCompletion("turn_direct_stopping", "hello");

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->state, GatewayAcceptedTurnTerminalState::kFailed);
    ASSERT_TRUE(result->failure.has_value());
    EXPECT_EQ(result->failure->code, "server_stopping");
    EXPECT_EQ(result->failure->message, "server stopping");
    EXPECT_FALSE(result->reply_text.has_value());

    const std::vector<EmittedEvent> events = session().events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_direct_stopping");
    EXPECT_EQ(events[0].payload, "server_stopping:server stopping");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_direct_stopping");
}

TEST_F(GatewayStubResponderTest, AcceptedTurnUpdatesSessionMemoryPrompt) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- [user | "), std::string::npos);
    EXPECT_NE(prompt->find("] hello"), std::string::npos);
    EXPECT_NE(prompt->find("- [assistant | "), std::string::npos);
    EXPECT_NE(prompt->find("] stub echo: hello"), std::string::npos);
}

TEST_F(GatewayStubResponderTest, CancelBeforeReplyEmitsTurnCancelled) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.cancelled");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, CancelledTurnEmitsCancellationTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_cancel",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_cancel", telemetry_sink),
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_cancel",
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventTurnCancelledEmitStarted,
        telemetry::kEventTurnCancelledEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseEmitTurnCancelled,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeCancelled);
}

TEST_F(GatewayStubResponderTest, MismatchedCancelRequestDoesNotAffectTrackedTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_other",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, ServerStoppingEmitsErrorThenCompletionForPendingTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    responder_.OnServerStopping(registry_attachment_->registry());

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "server_stopping:server stopping");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, SessionClosedBeforeReplyDropsPendingTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    session_->MarkClosed();
    registry_attachment_->registry().OnSessionClosed(SessionClosedEvent{
        .session_id = "srv_test",
        .session_started = true,
        .inflight_turn_id = std::string("turn_1"),
        .reason = SessionCloseReason::TransportClosed,
        .detail = "client closed",
    });

    std::this_thread::sleep_for(60ms);
    EXPECT_TRUE(session_->events().empty());
}

TEST_F(GatewayStubResponderTest, EmitFailureDoesNotBlockLaterTurns) {
    session_->FailNextTextOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "first",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    {
        const std::vector<EmittedEvent> first_events = session_->events();
        ASSERT_EQ(first_events.size(), 2U);
        EXPECT_EQ(first_events[0].op, "error");
        EXPECT_EQ(first_events[0].turn_id, "turn_1");
        EXPECT_EQ(first_events[0].payload,
                  "internal_error:stub responder failed to emit text output");
        EXPECT_EQ(first_events[1].op, "turn.completed");
        EXPECT_EQ(first_events[1].turn_id, "turn_1");
    }

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "second",
    });

    ASSERT_TRUE(session_->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: second");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST_F(GatewayStubResponderTest, FailedTurnEmitsFailureTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    session_->FailNextTextOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_failure",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_failure", telemetry_sink),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventTurnFailed,
        telemetry::kEventErrorEmitStarted,
        telemetry::kEventErrorEmitCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseEmitError,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeFailed);
}

TEST_F(GatewayStubResponderTest, CompletionHookCanQueueNextTurnForSameSession) {
    session_->SetEventHook([this](const EmittedEvent& event) {
        if (event.op == "turn.completed" && event.turn_id == "turn_1") {
            responder_.OnTurnAccepted(TurnAcceptedEvent{
                .session_id = "srv_test",
                .turn_id = "turn_2",
                .text = "second",
            });
        }
    });

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "first",
    });

    ASSERT_TRUE(session_->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: second");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST_F(GatewayStubResponderTest, OversizedAcceptedTurnIsTerminalizedWithoutReply) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = std::string(kMaxTextInputBytes + 1U, 'x'),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "bad_request:text.input text exceeds maximum length");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, OversizedTurnStillCompletesWhenErrorEmitFails) {
    session_->FailNextErrorOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = std::string(kMaxTextInputBytes + 1U, 'x'),
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.completed");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST_P(GatewayStubResponderPromptBudgetTest, SecondLargeTurnRespectsConfiguredPromptBudget) {
    GatewayStubResponderConfig config = MakeEchoConfig();
    GetParam().configure(config);
    InitializeResponder(std::move(config));
    StartSession();

    const std::string large_turn_text = MakeLargeTurnText();
    AcceptTurn("turn_1", large_turn_text);
    ASSERT_TRUE(session().WaitForEventCount(2U));

    AcceptTurn("turn_2", large_turn_text);

    const std::vector<EmittedEvent> events = WaitForEvents(4U);
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, GetParam().expected_second_turn_op);
    EXPECT_EQ(events[2].turn_id, "turn_2");
    if (GetParam().expected_payload_is_prefix) {
        EXPECT_THAT(events[2].payload,
                    ::testing::StartsWith(GetParam().expected_second_turn_payload));
    } else {
        EXPECT_EQ(events[2].payload, GetParam().expected_second_turn_payload);
    }
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

INSTANTIATE_TEST_SUITE_P(
    PromptBudgetMatrix, GatewayStubResponderPromptBudgetTest,
    ::testing::Values(
        PromptBudgetCase{
            .name = "DefaultWorkingMemoryBudgetRejectsLargeContext",
            .configure = [](GatewayStubResponderConfig&) {},
            .expected_second_turn_op = "error",
            .expected_second_turn_payload =
                "bad_request:rendered working memory context exceeds maximum length",
        },
        PromptBudgetCase{
            .name = "RaisedWorkingMemoryBudgetAllowsLargeContext",
            .configure =
                [](GatewayStubResponderConfig& config) {
                    config.max_rendered_working_memory_context_bytes = 512U * 1024U;
                },
            .expected_second_turn_op = "text.output",
            .expected_second_turn_payload = "stub echo: ",
            .expected_payload_is_prefix = true,
        },
        PromptBudgetCase{
            .name = "SmallerCombinedPromptBudgetRejectsOtherwiseAllowedContext",
            .configure =
                [](GatewayStubResponderConfig& config) {
                    config.max_rendered_system_prompt_bytes = 512U * 1024U;
                    config.max_rendered_working_memory_context_bytes = 512U * 1024U;
                    config.max_rendered_prompt_bytes = 70U * 1024U;
                },
            .expected_second_turn_op = "error",
            .expected_second_turn_payload =
                "bad_request:rendered prompt exceeds maximum combined length",
        },
        PromptBudgetCase{
            .name = "LargerCombinedPromptBudgetDoesNotClampComponentBudgets",
            .configure =
                [](GatewayStubResponderConfig& config) {
                    config.max_rendered_system_prompt_bytes = 512U * 1024U;
                    config.max_rendered_working_memory_context_bytes = 512U * 1024U;
                    config.max_rendered_prompt_bytes = 2U * 1024U * 1024U;
                },
            .expected_second_turn_op = "text.output",
            .expected_second_turn_payload = "stub echo: ",
            .expected_payload_is_prefix = true,
        }),
    [](const ::testing::TestParamInfo<PromptBudgetCase>& info) { return info.param.name; });

TEST_F(GatewayStubResponderStandaloneFixture,
       ReplyBuilderExceptionTerminatesTurnAndWorkerContinues) {
    GatewayStubResponderConfig config = MakeEchoConfig();
    config.openai_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (IsMidTermMemoryRequest(request)) {
                return EmitMidTermAwareEchoResponse(request, on_event);
            }
            const std::string latest_text = test::ExtractLatestPromptLine(request.user_text);
            if (latest_text == "explode") {
                throw std::runtime_error("synthetic reply builder failure");
            }
            const std::string text = std::string("stub echo: ") + latest_text;
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
            if (!delta_status.ok()) {
                return delta_status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_test",
            });
        });
    InitializeResponder(std::move(config));
    StartSession();

    AcceptTurn("turn_1", "explode");

    {
        const std::vector<EmittedEvent> events = WaitForEvents(2U);
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[0].op, "error");
        EXPECT_EQ(events[0].turn_id, "turn_1");
        EXPECT_EQ(events[0].payload, "internal_error:stub responder processing failed");
        EXPECT_EQ(events[1].op, "turn.completed");
        EXPECT_EQ(events[1].turn_id, "turn_1");
    }

    AcceptTurn("turn_2", "ok");

    const std::vector<EmittedEvent> events = WaitForEvents(4U);
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: ok");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST_F(GatewayStubResponderStandaloneFixture, OpenAiProviderFailureEmitsMappedErrorAndCompletion) {
    GatewayStubResponderConfig config = MakeEchoConfig();
    config.openai_client =
        test::MakeFakeOpenAiResponsesClient(absl::UnavailableError("provider down"));
    InitializeResponder(std::move(config));
    StartSession();

    AcceptTurn("turn_1", "hello");

    const std::vector<EmittedEvent> events = WaitForEvents(2U);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "service_unavailable:upstream service unavailable");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderStandaloneFixture, AcceptedTurnWithoutSessionStartFailsClosed) {
    InitializeResponder(MakeEchoConfig());

    AcceptTurn("turn_1", "hello");

    const std::vector<EmittedEvent> events = WaitForEvents(2U);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderStandaloneFixture, SessionCloseAfterSessionStartRemovesEmptyMemory) {
    InitializeResponder(MakeEchoConfig());
    StartSession();

    ASSERT_TRUE(responder().RenderSessionMemoryPrompt(session_id()).ok());

    responder().OnSessionClosed(SessionClosedEvent{
        .session_id = session_id(),
        .session_started = true,
        .inflight_turn_id = std::nullopt,
        .reason = SessionCloseReason::ProtocolEnded,
        .detail = "session ended",
    });

    const absl::StatusOr<std::string> prompt = responder().RenderSessionMemoryPrompt(session_id());
    EXPECT_FALSE(prompt.ok());
    EXPECT_EQ(prompt.status().code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace isla::server::ai_gateway
