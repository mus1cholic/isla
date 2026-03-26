#include "ai_gateway_stub_responder_test_support.hpp"

#include <future>
#include <mutex>
#include <thread>

namespace isla::server::ai_gateway {
namespace {

using namespace test_support;

using ExtraMidTermRequestHandler = std::function<std::optional<absl::Status>(
    const OpenAiResponsesRequest&, const OpenAiResponsesEventCallback&)>;

std::shared_ptr<test::FakeOpenAiResponsesClient> MakeRecordingMidTermMemoryClient(
    const std::shared_ptr<std::vector<test::OpenAiResponsesRequestSnapshot>>& recorded_requests,
    const std::shared_ptr<std::mutex>& requests_mutex,
    const std::shared_ptr<std::promise<void>>& compactor_finished,
    const std::shared_ptr<std::once_flag>& compactor_finished_once,
    ExtraMidTermRequestHandler extra_request_handler = {}) {
    auto decider_call_count = std::make_shared<std::atomic<int>>(0);
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [recorded_requests, requests_mutex, compactor_finished, compactor_finished_once,
         decider_call_count, extra_request_handler = std::move(extra_request_handler)](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            {
                std::lock_guard<std::mutex> lock(*requests_mutex);
                recorded_requests->push_back(test::TakeRequestSnapshot(request));
            }
            if (request.system_prompt == MidTermFlushDeciderPromptText()) {
                const int call_index = decider_call_count->fetch_add(1);
                if (call_index == 0) {
                    return EmitResponseText(R"json({
                        "should_flush": true,
                        "item_id": "i0",
                        "split_at": null,
                        "reasoning": "Completed first exchange."
                    })json",
                                            on_event, "resp_decider");
                }
                return EmitResponseText(R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No additional completed episode."
                })json",
                                        on_event, "resp_decider");
            }
            if (request.system_prompt == MidTermCompactorPromptText()) {
                const absl::Status status = EmitResponseText(R"json({
                    "tier1_detail": "First exchange detail.",
                    "tier2_summary": "First exchange summary.",
                    "tier3_ref": "First exchange ref.",
                    "tier3_keywords": ["first", "exchange", "summary", "memory", "test"],
                    "salience": 8
                })json",
                                                             on_event, "resp_compactor");
                if (status.ok()) {
                    std::call_once(*compactor_finished_once,
                                   [compactor_finished] { compactor_finished->set_value(); });
                }
                return status;
            }
            if (extra_request_handler) {
                std::optional<absl::Status> extra_status = extra_request_handler(request, on_event);
                if (extra_status.has_value()) {
                    return *extra_status;
                }
            }

            const std::string reply =
                std::string("stub echo: ") + test::ExtractLatestPromptLine(request.user_text);
            return EmitResponseText(reply, on_event);
        });
}

std::vector<test::OpenAiResponsesRequestSnapshot> CopyRecordedRequests(
    const std::shared_ptr<std::vector<test::OpenAiResponsesRequestSnapshot>>& recorded_requests,
    const std::shared_ptr<std::mutex>& requests_mutex) {
    std::lock_guard<std::mutex> lock(*requests_mutex);
    return *recorded_requests;
}

TEST(GatewayStubResponderStandaloneTest,
     MidTermMemoryWiringEventuallyFlushesCompletedTurnIntoLaterPrompt) {
    auto recorded_requests = std::make_shared<std::vector<test::OpenAiResponsesRequestSnapshot>>();
    auto requests_mutex = std::make_shared<std::mutex>();
    auto compactor_finished = std::make_shared<std::promise<void>>();
    std::future<void> compactor_finished_future = compactor_finished->get_future();
    auto compactor_finished_once = std::make_shared<std::once_flag>();
    auto client = MakeRecordingMidTermMemoryClient(recorded_requests, requests_mutex,
                                                   compactor_finished, compactor_finished_once);

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .openai_client = client,
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
    ASSERT_TRUE(session->WaitForEventCount(2U));
    ASSERT_EQ(compactor_finished_future.wait_for(2s), std::future_status::ready);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "follow up",
    });
    ASSERT_TRUE(session->WaitForEventCount(4U));

    auto prompt_contains_mid_term_summary = [&]() -> bool {
        const absl::StatusOr<std::string> prompt = responder.RenderSessionMemoryPrompt("srv_test");
        if (!prompt.ok()) {
            return false;
        }
        return prompt->find("First exchange summary.") != std::string::npos &&
               prompt->find("First exchange ref.") != std::string::npos;
    };

    std::size_t expected_event_count = 4U;
    int turn_number = 3;
    while (turn_number <= 8 && !prompt_contains_mid_term_summary()) {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_" + std::to_string(turn_number),
            .text = turn_number == 3 ? "third turn" : "later turn " + std::to_string(turn_number),
        });
        expected_event_count += 2U;
        ASSERT_TRUE(session->WaitForEventCount(expected_event_count));
        ++turn_number;
    }
    ASSERT_TRUE(prompt_contains_mid_term_summary());

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_visibility_probe",
        .text = "visibility probe",
    });
    expected_event_count += 2U;
    ASSERT_TRUE(session->WaitForEventCount(expected_event_count));

    const std::vector<test::OpenAiResponsesRequestSnapshot> requests =
        CopyRecordedRequests(recorded_requests, requests_mutex);

    const auto decider_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.system_prompt == MidTermFlushDeciderPromptText();
        });
    ASSERT_NE(decider_request, requests.end());
    EXPECT_EQ(decider_request->model, kDefaultMidTermMemoryModel);

    const auto compactor_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.system_prompt == MidTermCompactorPromptText();
        });
    ASSERT_NE(compactor_request, requests.end());
    EXPECT_EQ(compactor_request->model, kDefaultMidTermMemoryModel);

    const auto later_reply_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.system_prompt != MidTermFlushDeciderPromptText() &&
                   request.system_prompt != MidTermCompactorPromptText() &&
                   request.user_text.find("ep_srv_test_1") != std::string::npos;
        });
    ASSERT_NE(later_reply_request, requests.end());
    EXPECT_NE(later_reply_request->user_text.find("<mid_term_episodes>"), std::string::npos);
    EXPECT_EQ(later_reply_request->user_text.find("<mid_term_episodes>\n- (none)"),
              std::string::npos);
    EXPECT_NE(later_reply_request->user_text.find("ep_srv_test_1"), std::string::npos);

    const absl::StatusOr<std::string> prompt = responder.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("First exchange summary."), std::string::npos);
    EXPECT_NE(prompt->find("First exchange ref."), std::string::npos);
}

TEST_F(GatewayStubResponderStandaloneFixture,
       AwaitSessionMemorySettledBlocksUntilCompactionDrains) {
    std::promise<void> release_promise;
    auto release_signal = release_promise.get_future().share();
    auto compactor_entered = std::make_shared<std::promise<void>>();
    std::future<void> compactor_entered_future = compactor_entered->get_future();
    auto compactor_entered_once = std::make_shared<std::once_flag>();

    auto client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [release_signal, compactor_entered,
         compactor_entered_once](const OpenAiResponsesRequest& request,
                                 const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.system_prompt == MidTermFlushDeciderPromptText()) {
                return EmitResponseText(R"json({
                    "should_flush": true,
                    "item_id": "i0",
                    "split_at": null,
                    "reasoning": "Completed first exchange."
                })json",
                                        on_event, "resp_decider");
            }
            if (request.system_prompt == MidTermCompactorPromptText()) {
                std::call_once(*compactor_entered_once,
                               [compactor_entered] { compactor_entered->set_value(); });
                release_signal.wait();
                return EmitResponseText(R"json({
                    "tier1_detail": "Settled detail.",
                    "tier2_summary": "Settled summary.",
                    "tier3_ref": "Settled ref.",
                    "tier3_keywords": ["settled", "memory", "test", "await", "drain"],
                    "salience": 7
                })json",
                                        on_event, "resp_compactor");
            }
            const std::string reply =
                std::string("stub echo: ") + test::ExtractLatestPromptLine(request.user_text);
            return EmitResponseText(reply, on_event);
        });

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.openai_client = client;
    InitializeResponder(std::move(config));
    StartSession();

    const absl::StatusOr<GatewayAcceptedTurnResult> turn_result =
        RunAcceptedTurnToCompletion("turn_settle", "hello");
    ASSERT_TRUE(turn_result.ok()) << turn_result.status();
    EXPECT_EQ(turn_result->state, GatewayAcceptedTurnTerminalState::kSucceeded);

    ASSERT_EQ(compactor_entered_future.wait_for(2s), std::future_status::ready);

    std::atomic<bool> settled_finished{ false };
    absl::Status settled_result;
    std::thread settle_thread([&] {
        settled_result = responder().AwaitSessionMemorySettled(session_id());
        settled_finished.store(true);
    });

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(settled_finished.load());

    release_promise.set_value();
    settle_thread.join();

    ASSERT_TRUE(settled_finished.load());
    ASSERT_TRUE(settled_result.ok()) << settled_result;

    const absl::StatusOr<isla::server::memory::WorkingMemoryState> state =
        responder().SnapshotSessionWorkingMemoryState(session_id());
    ASSERT_TRUE(state.ok()) << state.status();
    EXPECT_EQ(state->mid_term_episodes.size(), 1U);
    EXPECT_EQ(state->mid_term_episodes.front().tier2_summary, "Settled summary.");
}

TEST_F(GatewayStubResponderStandaloneFixture,
       AwaitSessionMemorySettledReturnsNotFoundForUnknownSession) {
    InitializeResponder(MakeEchoConfig());

    const absl::Status settled = responder().AwaitSessionMemorySettled("unknown_session");
    EXPECT_EQ(settled.code(), absl::StatusCode::kNotFound);
}

TEST_F(GatewayStubResponderStandaloneFixture, ExpandMidTermToolLoopUsesSessionMemoryDetail) {
    auto recorded_requests = std::make_shared<std::vector<test::OpenAiResponsesRequestSnapshot>>();
    auto requests_mutex = std::make_shared<std::mutex>();
    auto compactor_finished = std::make_shared<std::promise<void>>();
    std::future<void> compactor_finished_future = compactor_finished->get_future();
    auto compactor_finished_once = std::make_shared<std::once_flag>();
    auto tool_probe_round = std::make_shared<int>(0);
    auto client = MakeRecordingMidTermMemoryClient(
        recorded_requests, requests_mutex, compactor_finished, compactor_finished_once,
        [tool_probe_round](
            const OpenAiResponsesRequest& request,
            const OpenAiResponsesEventCallback& on_event) -> std::optional<absl::Status> {
            if (test::ExtractLatestPromptLine(request.user_text) == "need exact detail" &&
                request.input_items.empty()) {
                EXPECT_EQ(request.function_tools.size(), 1U);
                EXPECT_EQ(request.function_tools[0].name, "expand_mid_term");
                ++(*tool_probe_round);
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_tool_call",
                    .output_items =
                        {
                            OpenAiResponsesOutputItem{
                                .type = "function_call",
                                .raw_json = R"json({"type":"function_call","call_id":"call_expand_1","name":"expand_mid_term","arguments":"{\"episode_id\":\"ep_srv_test_1\"}"})json",
                                .call_id = std::string("call_expand_1"),
                                .name = std::string("expand_mid_term"),
                                .arguments_json =
                                    std::string(R"json({"episode_id":"ep_srv_test_1"})json"),
                            },
                        },
                });
            }

            if (request.user_text.empty() && request.input_items.size() == 2U &&
                std::holds_alternative<OpenAiResponsesFunctionCallOutputInputItem>(
                    request.input_items[1])) {
                const auto& tool_output =
                    std::get<OpenAiResponsesFunctionCallOutputInputItem>(request.input_items[1]);
                EXPECT_EQ(tool_output.call_id, "call_expand_1");
                EXPECT_EQ(tool_output.output, "First exchange detail.");
                ++(*tool_probe_round);
                return EmitResponseText("tool answer: First exchange detail.", on_event,
                                        "resp_tool_final");
            }

            return std::nullopt;
        });

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.openai_client = client;
    InitializeResponder(std::move(config));
    StartSession();

    AcceptTurn("turn_1", "hello");
    ASSERT_TRUE(session().WaitForEventCount(2U));
    ASSERT_EQ(compactor_finished_future.wait_for(2s), std::future_status::ready);

    auto prompt_contains_mid_term_summary = [&]() -> bool {
        const absl::StatusOr<std::string> prompt =
            responder().RenderSessionMemoryPrompt(session_id());
        if (!prompt.ok()) {
            return false;
        }
        return prompt->find("First exchange summary.") != std::string::npos &&
               prompt->find("First exchange ref.") != std::string::npos;
    };

    std::size_t expected_event_count = 2U;
    int turn_number = 2;
    while (turn_number <= 8 && !prompt_contains_mid_term_summary()) {
        AcceptTurn("turn_" + std::to_string(turn_number),
                   turn_number == 2 ? "follow up" : "later turn " + std::to_string(turn_number));
        expected_event_count += 2U;
        ASSERT_TRUE(session().WaitForEventCount(expected_event_count));
        ++turn_number;
    }
    ASSERT_TRUE(prompt_contains_mid_term_summary());

    AcceptTurn("turn_expand_probe", "need exact detail");
    expected_event_count += 2U;
    ASSERT_TRUE(session().WaitForEventCount(expected_event_count));

    const std::vector<EmittedEvent> events = session().events();
    ASSERT_GE(events.size(), 2U);
    EXPECT_EQ(events[events.size() - 2U].op, "text.output");
    EXPECT_EQ(events[events.size() - 2U].payload, "tool answer: First exchange detail.");
    EXPECT_EQ(events.back().op, "turn.completed");
    EXPECT_EQ(events.back().turn_id, "turn_expand_probe");

    EXPECT_EQ(*tool_probe_round, 2);

    const std::vector<test::OpenAiResponsesRequestSnapshot> requests =
        CopyRecordedRequests(recorded_requests, requests_mutex);
    const auto replay_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.user_text.empty() && request.input_items.size() == 2U &&
                   std::holds_alternative<OpenAiResponsesFunctionCallOutputInputItem>(
                       request.input_items[1]);
        });
    ASSERT_NE(replay_request, requests.end());
    ASSERT_TRUE(
        std::holds_alternative<OpenAiResponsesRawInputItem>(replay_request->input_items[0]));
    const auto& tool_output =
        std::get<OpenAiResponsesFunctionCallOutputInputItem>(replay_request->input_items[1]);
    EXPECT_EQ(tool_output.call_id, "call_expand_1");
    EXPECT_EQ(tool_output.output, "First exchange detail.");
}

TEST_F(GatewayStubResponderStandaloneFixture, MidTermMemoryUsesConfiguredModelOverrides) {
    auto recorded_requests = std::make_shared<std::vector<test::OpenAiResponsesRequestSnapshot>>();
    auto requests_mutex = std::make_shared<std::mutex>();
    auto compactor_finished = std::make_shared<std::promise<void>>();
    std::future<void> compactor_finished_future = compactor_finished->get_future();
    auto compactor_finished_once = std::make_shared<std::once_flag>();
    auto client = MakeRecordingMidTermMemoryClient(recorded_requests, requests_mutex,
                                                   compactor_finished, compactor_finished_once);

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.llm_runtime_config = GatewayLlmRuntimeConfig{
        .main_model = std::string(kDefaultMainLlmModel),
        .mid_term_flush_decider_model = "gpt-4.1-mini",
        .mid_term_compactor_model = "gpt-4.1-nano",
    };
    config.openai_client = client;
    InitializeResponder(std::move(config));
    StartSession();

    AcceptTurn("turn_1", "hello");
    ASSERT_TRUE(session().WaitForEventCount(2U));
    ASSERT_EQ(compactor_finished_future.wait_for(2s), std::future_status::ready);

    const std::vector<test::OpenAiResponsesRequestSnapshot> requests =
        CopyRecordedRequests(recorded_requests, requests_mutex);

    const auto decider_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.system_prompt == MidTermFlushDeciderPromptText();
        });
    ASSERT_NE(decider_request, requests.end());
    EXPECT_EQ(decider_request->model, "gpt-4.1-mini");

    const auto compactor_request =
        std::find_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.system_prompt == MidTermCompactorPromptText();
        });
    ASSERT_NE(compactor_request, requests.end());
    EXPECT_EQ(compactor_request->model, "gpt-4.1-nano");
}

TEST_F(GatewayStubResponderStandaloneFixture,
       MidTermMemoryNotConfiguredStillAllowsSessionMemoryStartup) {
    GatewayStubResponderConfig config{};
    config.response_delay = 0ms;
    config.async_emit_timeout = 2s;
    InitializeResponder(std::move(config));

    EXPECT_FALSE(responder().IsMidTermMemoryConfigured());
    EXPECT_FALSE(responder().IsMidTermMemoryAvailable());
    EXPECT_TRUE(responder().MidTermMemoryInitializationStatus().ok());

    StartSession();
    ASSERT_TRUE(responder().RenderSessionMemoryPrompt(session_id()).ok());
    EXPECT_TRUE(session().events().empty());
}

TEST_F(GatewayStubResponderStandaloneFixture, TranscriptSeedDrainsCompactionBeforeNextSeed) {
    // Track the order of decider/compactor calls and verify that when the flush
    // decider fires a second time (on a later seed), the first compaction has
    // already been applied — i.e. the conversation items have been compacted,
    // not left as raw ongoing episodes.

    auto decider_call_count = std::make_shared<std::atomic<int>>(0);

    auto client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [decider_call_count](const OpenAiResponsesRequest& request,
                             const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.system_prompt == MidTermFlushDeciderPromptText()) {
                const int call_index = decider_call_count->fetch_add(1);
                // Return the last conversation item (the active ongoing episode).
                // After the first flush, prior items become stubs, so the ongoing
                // episode shifts to a higher index.
                const std::string item_id = "i" + std::to_string(call_index);
                const std::string response = R"json({
                    "should_flush": true,
                    "item_id": ")json" + item_id +
                                             R"json(",
                    "split_at": null,
                    "reasoning": "Completed exchange."
                })json";
                return EmitResponseText(response, on_event, "resp_decider");
            }
            if (request.system_prompt == MidTermCompactorPromptText()) {
                return EmitResponseText(R"json({
                    "tier1_detail": "Seed episode detail.",
                    "tier2_summary": "Seed episode summary.",
                    "tier3_ref": "Seed episode ref.",
                    "tier3_keywords": ["seed", "transcript", "replay", "memory", "test"],
                    "salience": 6
                })json",
                                        on_event, "resp_compactor");
            }
            const std::string reply =
                std::string("stub echo: ") + test::ExtractLatestPromptLine(request.user_text);
            return EmitResponseText(reply, on_event);
        });

    GatewayStubResponderConfig config = MakeEchoConfig();
    config.openai_client = client;
    InitializeResponder(std::move(config));
    StartSession();

    // Replay several pairs of transcript seeds.  Each assistant reply should
    // trigger the flush decider, and HandleTranscriptSeed should block until the
    // compaction is applied before accepting the next seed.
    for (int pair = 0; pair < 3; ++pair) {
        const std::string turn_id = "seed_" + std::to_string(pair);
        const absl::Status user_status = responder().HandleTranscriptSeed(TranscriptSeedEvent{
            .session_id = session_id(),
            .turn_id = turn_id,
            .role = "user",
            .text = "user message " + std::to_string(pair),
        });
        ASSERT_TRUE(user_status.ok()) << "pair=" << pair << " user seed: " << user_status;
        const absl::Status assistant_status = responder().HandleTranscriptSeed(TranscriptSeedEvent{
            .session_id = session_id(),
            .turn_id = turn_id,
            .role = "assistant",
            .text = "assistant reply " + std::to_string(pair),
        });
        ASSERT_TRUE(assistant_status.ok())
            << "pair=" << pair << " assistant seed: " << assistant_status;
    }

    // The decider should have been called more than once — once per completed
    // exchange — because each compaction was drained before the next seed pair
    // was processed.  Without the synchronous drain, the second and third calls
    // would be skipped (pending_mid_term_flushes_ would be non-empty).
    EXPECT_GE(decider_call_count->load(), 2);

    // Verify the memory is settled: there should be compacted mid-term episodes.
    const absl::StatusOr<isla::server::memory::WorkingMemoryState> state =
        responder().SnapshotSessionWorkingMemoryState(session_id());
    ASSERT_TRUE(state.ok()) << state.status();
    EXPECT_GE(state->mid_term_episodes.size(), 1U);
}

} // namespace
} // namespace isla::server::ai_gateway
