#include "memory_orchestrator_test_support.hpp"

namespace isla::server::memory {
namespace {

TEST_F(MemoryOrchestratorTest, DrainCompletedMidTermCompactionsReturnsZeroWhenNothingQueued) {
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandler();
    ASSERT_TRUE(handler.ok()) << handler.status();

    const absl::StatusOr<std::size_t> drained = handler->DrainCompletedMidTermCompactions();

    ASSERT_TRUE(drained.ok()) << drained.status();
    EXPECT_EQ(*drained, 0U);
}

TEST_F(MemoryOrchestratorTest, AwaitAndDrainReturnsZeroWhenNothingPending) {
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandler();
    ASSERT_TRUE(handler.ok()) << handler.status();

    const absl::StatusOr<std::size_t> drained =
        handler->AwaitAndDrainAllPendingMidTermCompactions();

    ASSERT_TRUE(drained.ok()) << drained.status();
    EXPECT_EQ(*drained, 0U);
}

TEST_F(MemoryOrchestratorTest, AwaitAndDrainBlocksUntilPendingCompactionCompletes) {
    std::promise<void> release_promise;
    auto release_signal = release_promise.get_future().share();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_signal);

    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));
    EXPECT_TRUE(handler->HasPendingMidTermCompactions());

    // Start AwaitAndDrain on another thread while the compactor is still blocked.
    std::atomic<bool> await_finished{ false };
    absl::StatusOr<std::size_t> drained_result;
    std::thread await_thread([&] {
        drained_result = handler->AwaitAndDrainAllPendingMidTermCompactions();
        await_finished.store(true);
    });

    // Give the await thread time to enter the blocking wait, then verify it hasn't returned.
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(await_finished.load());

    // Release the compactor — the await thread should now unblock and drain.
    release_promise.set_value();
    await_thread.join();

    ASSERT_TRUE(await_finished.load());
    ASSERT_TRUE(drained_result.ok()) << drained_result.status();
    EXPECT_EQ(*drained_result, 1U);
    EXPECT_FALSE(handler->HasPendingMidTermCompactions());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_EQ(state.mid_term_episodes.size(), 1U);
}

TEST_F(MemoryOrchestratorTest, AwaitAndDrainPropagatesCompactorFailure) {
    auto compactor =
        std::make_shared<RecordingMidTermCompactor>(absl::InternalError("compaction failed"));
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    const absl::StatusOr<std::size_t> drained =
        handler->AwaitAndDrainAllPendingMidTermCompactions();
    ASSERT_FALSE(drained.ok());
    EXPECT_EQ(drained.status().code(), absl::StatusCode::kInternal);
}

TEST_F(MemoryOrchestratorTest, ConversationStaysOnSingleEpisodeWhenNoCompactorConfigured) {
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandler();
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_002", "follow up",
                                                       Ts("2026-03-08T14:00:02Z")))
                    .ok());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    ASSERT_EQ(state.conversation.items.size(), 1U);
    ASSERT_TRUE(state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[0].ongoing_episode->messages.size(), 3U);
    EXPECT_EQ(state.conversation.items[0].ongoing_episode->messages[2].content, "follow up");
}

TEST_F(MemoryOrchestratorTest, FlushDeciderCanSuppressAutomaticFlushQueueing) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(NoFlushDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(decider->WaitForRequestCount(1U));
    EXPECT_TRUE(compactor->requests().empty());
    ASSERT_EQ(decider->requests().size(), 1U);

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_002", "follow up",
                                                       Ts("2026-03-08T14:00:02Z")))
                    .ok());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    ASSERT_EQ(state.conversation.items.size(), 1U);
    ASSERT_TRUE(state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[0].ongoing_episode->messages.size(), 3U);
}

TEST_F(MemoryOrchestratorTest,
       FlushDeciderUsesDefaultCadenceAndWaitsForTenUserTurnsBeforeQueueingAnalysis) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());

    absl::StatusOr<MemoryOrchestrator> handler =
        MemoryOrchestrator::Create("srv_test", MemoryOrchestratorInit{
                                                   .user_id = "user_001",
                                                   .store = nullptr,
                                                   .mid_term_flush_decider = decider,
                                                   .mid_term_compactor = compactor,
                                               });
    ASSERT_TRUE(handler.ok()) << handler.status();

    for (int turn = 1; turn <= 9; ++turn) {
        ASSERT_TRUE(handler
                        ->HandleUserQuery(GatewayUserQuery(
                            "srv_test", "turn_u_" + std::to_string(turn),
                            "u" + std::to_string(turn), Ts("2026-03-08T14:00:00Z")))
                        .ok());
        ASSERT_TRUE(handler
                        ->HandleAssistantReply(GatewayAssistantReply(
                            "srv_test", "turn_a_" + std::to_string(turn),
                            "a" + std::to_string(turn), Ts("2026-03-08T14:00:01Z")))
                        .ok());
    }

    EXPECT_TRUE(decider->requests().empty());
    EXPECT_TRUE(compactor->requests().empty());

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_u_10", "u10",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_a_10", "a10",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    ASSERT_TRUE(decider->WaitForRequestCount(1U));
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));
}

TEST_F(MemoryOrchestratorTest, CreateRejectsZeroFlushDeciderCadence) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());

    const absl::StatusOr<MemoryOrchestrator> handler =
        MemoryOrchestrator::Create("srv_test", MemoryOrchestratorInit{
                                                   .user_id = "user_001",
                                                   .store = nullptr,
                                                   .mid_term_flush_decider = decider,
                                                   .mid_term_compactor = compactor,
                                                   .mid_term_flush_decider_interval_user_turns = 0U,
                                               });

    ASSERT_FALSE(handler.ok());
    EXPECT_EQ(handler.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(MemoryOrchestratorTest, FlushDeciderCanChooseConversationItemForAsyncFlush) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    const std::vector<MidTermCompactionRequest> requests = compactor->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].flush_candidate.conversation_item_index, 0U);
    ASSERT_EQ(requests[0].flush_candidate.ongoing_episode.messages.size(), 2U);
}

TEST_F(MemoryOrchestratorTest, FullFlushDecisionRebasesToSplitWhenNewMessagesArriveBeforeDrain) {
    std::promise<void> release_promise;
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_promise.get_future().share());
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_001", "u1", Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "a1",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_002", "u2", Ts("2026-03-08T14:00:02Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_002", "a2",
                                                                 Ts("2026-03-08T14:00:03Z")))
                    .ok());

    const WorkingMemoryState& pending_state = handler->memory().snapshot();
    ASSERT_EQ(pending_state.conversation.items.size(), 1U);
    ASSERT_TRUE(pending_state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(pending_state.conversation.items[0].ongoing_episode->messages.size(), 4U);

    release_promise.set_value();
    const absl::StatusOr<std::size_t> drained = WaitForDrain(*handler, 1U);
    ASSERT_TRUE(drained.ok()) << drained.status();

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    ASSERT_EQ(state.conversation.items.size(), 2U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[0].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[0].episode_stub->content, "stub ref");
    EXPECT_EQ(state.conversation.items[1].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(state.conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[1].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(state.conversation.items[1].ongoing_episode->messages[0].content, "u2");
    EXPECT_EQ(state.conversation.items[1].ongoing_episode->messages[1].content, "a2");
}

TEST_F(MemoryOrchestratorTest, NoFlushAnalysisDrainsCleanlyAndLaterAnalysisCanRunAgain) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(NoFlushDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_001", "u1", Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "a1",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(decider->WaitForRequestCount(1U));
    EXPECT_TRUE(compactor->requests().empty());

    decider->SetDecision(TailCompleteDecision());

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_002", "u2", Ts("2026-03-08T14:00:02Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_002", "a2",
                                                                 Ts("2026-03-08T14:00:03Z")))
                    .ok());

    ASSERT_TRUE(decider->WaitForRequestCount(2U));
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));
    EXPECT_EQ(decider->requests().size(), 2U);
    EXPECT_EQ(compactor->requests().size(), 1U);

    const absl::StatusOr<std::size_t> drained = WaitForDrain(*handler, 1U);
    ASSERT_TRUE(drained.ok()) << drained.status();

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    EXPECT_EQ(state.mid_term_episodes[0].episode_id, "ep_srv_test_1");
}

TEST_F(MemoryOrchestratorTest, NextTurnPropagatesPendingAnalysisFailureBeforeMutation) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider =
        std::make_shared<RecordingMidTermFlushDecider>(absl::InternalError("decider failed"));
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_001", "u1", Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "a1",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(decider->WaitForRequestCount(1U));

    const WorkingMemoryState& before_state = handler->memory().snapshot();
    ASSERT_EQ(before_state.conversation.items.size(), 1U);
    ASSERT_TRUE(before_state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(before_state.conversation.items[0].ongoing_episode->messages.size(), 2U);

    const absl::StatusOr<UserQueryMemoryResult> result = handler->HandleUserQuery(
        GatewayUserQuery("srv_test", "turn_002", "u2", Ts("2026-03-08T14:00:02Z")));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);

    const WorkingMemoryState& after_state = handler->memory().snapshot();
    ASSERT_EQ(after_state.conversation.items.size(), 1U);
    ASSERT_TRUE(after_state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(after_state.conversation.items[0].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(after_state.conversation.items[0].ongoing_episode->messages[0].content, "u1");
    EXPECT_EQ(after_state.conversation.items[0].ongoing_episode->messages[1].content, "a1");
}

TEST_F(MemoryOrchestratorTest,
       AsyncAnalysisUsesConversationSnapshotEvenWhenLiveConversationChanges) {
    std::promise<void> release_promise;
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_promise.get_future().share());
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_001", "u1", Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "a1",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(decider->WaitForRequestCount(1U));
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_002", "u2", Ts("2026-03-08T14:00:02Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_002", "a2",
                                                                 Ts("2026-03-08T14:00:03Z")))
                    .ok());

    const std::vector<Conversation> decider_requests = decider->requests();
    ASSERT_EQ(decider_requests.size(), 1U);
    ASSERT_EQ(decider_requests[0].items.size(), 1U);
    ASSERT_TRUE(decider_requests[0].items[0].ongoing_episode.has_value());
    ASSERT_EQ(decider_requests[0].items[0].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(decider_requests[0].items[0].ongoing_episode->messages[0].content, "u1");
    EXPECT_EQ(decider_requests[0].items[0].ongoing_episode->messages[1].content, "a1");

    const std::vector<MidTermCompactionRequest> compactor_requests = compactor->requests();
    ASSERT_EQ(compactor_requests.size(), 1U);
    ASSERT_EQ(compactor_requests[0].flush_candidate.ongoing_episode.messages.size(), 2U);
    EXPECT_EQ(compactor_requests[0].flush_candidate.ongoing_episode.messages[0].content, "u1");
    EXPECT_EQ(compactor_requests[0].flush_candidate.ongoing_episode.messages[1].content, "a1");

    const WorkingMemoryState& live_state = handler->memory().snapshot();
    ASSERT_EQ(live_state.conversation.items.size(), 1U);
    ASSERT_TRUE(live_state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(live_state.conversation.items[0].ongoing_episode->messages.size(), 4U);

    release_promise.set_value();
    ASSERT_TRUE(WaitForDrain(*handler, 1U).ok());
}

TEST_F(MemoryOrchestratorTest, DrainSucceedsWhenRebasedFullFlushLeavesAssistantTail) {
    std::promise<void> release_promise;
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_promise.get_future().share());
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(
                        GatewayUserQuery("srv_test", "turn_001", "u1", Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "a1",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    AppendAssistantMessage(handler->mutable_memory().mutable_conversation(), "bad assistant append",
                           Ts("2026-03-08T14:00:02Z"));

    release_promise.set_value();
    const absl::StatusOr<std::size_t> drained = WaitForDrain(*handler, 1U);
    ASSERT_TRUE(drained.ok()) << drained.status();

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    ASSERT_EQ(state.conversation.items.size(), 2U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[1].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(state.conversation.items[1].ongoing_episode->messages[0].role,
              MessageRole::Assistant);
    EXPECT_EQ(state.conversation.items[1].ongoing_episode->messages[0].content,
              "bad assistant append");
}

TEST_F(MemoryOrchestratorTest, HandleAssistantReplyPropagatesFlushDeciderFailure) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    auto decider =
        std::make_shared<RecordingMidTermFlushDecider>(absl::InternalError("decider failed"));
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());

    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    ASSERT_TRUE(WaitForDrainFailure(*handler, absl::StatusCode::kInternal).ok());
    EXPECT_TRUE(compactor->requests().empty());
}

TEST_F(MemoryOrchestratorTest, FlushDeciderDoesNotQueueDuplicatePendingFlushes) {
    std::promise<void> release_promise;
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_promise.get_future().share());
    auto decider = std::make_shared<RecordingMidTermFlushDecider>(TailCompleteDecision());
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, nullptr, decider);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply(
                        "srv_test", "turn_002", "extra reply", Ts("2026-03-08T14:00:02Z")))
                    .ok());

    EXPECT_EQ(compactor->requests().size(), 1U);
    release_promise.set_value();
}

TEST_F(MemoryOrchestratorTest, NextUserTurnDrainsCompletedAsyncFlushIntoMidTermMemory) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_002", "follow up",
                                                       Ts("2026-03-08T14:00:02Z")))
                    .ok());

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    EXPECT_EQ(state.mid_term_episodes[0].episode_id, "ep_srv_test_1");
    EXPECT_EQ(state.mid_term_episodes[0].tier2_summary, "summary");
    ASSERT_EQ(state.conversation.items.size(), 2U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    ASSERT_TRUE(state.conversation.items[0].episode_stub.has_value());
    EXPECT_EQ(state.conversation.items[0].episode_stub->content, "stub ref");
    EXPECT_EQ(state.conversation.items[1].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(state.conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[1].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(state.conversation.items[1].ongoing_episode->messages[0].content, "follow up");
}

TEST_F(MemoryOrchestratorTest, PendingAsyncFlushStartsNewEpisodeBeforeNextUserMessageAppends) {
    std::promise<void> release_promise;
    auto compactor = std::make_shared<RecordingMidTermCompactor>(
        CompactedMidTermEpisode{
            .tier1_detail = std::string("full detail"),
            .tier2_summary = "summary",
            .tier3_ref = "stub ref",
            .tier3_keywords = { "memory" },
            .salience = kExpandableEpisodeSalienceThreshold,
            .embedding = {},
        },
        release_promise.get_future().share());
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_002", "follow up",
                                                       Ts("2026-03-08T14:00:02Z")))
                    .ok());

    const WorkingMemoryState& pending_state = handler->memory().snapshot();
    ASSERT_EQ(pending_state.mid_term_episodes.size(), 0U);
    ASSERT_EQ(pending_state.conversation.items.size(), 2U);
    EXPECT_EQ(pending_state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(pending_state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(pending_state.conversation.items[0].ongoing_episode->messages.size(), 2U);
    EXPECT_EQ(pending_state.conversation.items[1].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(pending_state.conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(pending_state.conversation.items[1].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(pending_state.conversation.items[1].ongoing_episode->messages[0].content,
              "follow up");

    release_promise.set_value();
    const absl::StatusOr<std::size_t> drained = WaitForDrain(*handler, 1U);
    ASSERT_TRUE(drained.ok()) << drained.status();
    EXPECT_EQ(*drained, 1U);

    const WorkingMemoryState& drained_state = handler->memory().snapshot();
    ASSERT_EQ(drained_state.mid_term_episodes.size(), 1U);
    EXPECT_EQ(drained_state.conversation.items[0].type, ConversationItemType::EpisodeStub);
    EXPECT_EQ(drained_state.conversation.items[1].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(drained_state.conversation.items[1].ongoing_episode.has_value());
    ASSERT_EQ(drained_state.conversation.items[1].ongoing_episode->messages.size(), 1U);
    EXPECT_EQ(drained_state.conversation.items[1].ongoing_episode->messages[0].content,
              "follow up");
}

TEST_F(MemoryOrchestratorTest, DrainCompletedMidTermCompactionsRejectsInvalidCompactorOutput) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "memory" },
        .salience = kExpandableEpisodeSalienceThreshold,
        .embedding = {},
    });
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    ASSERT_TRUE(WaitForDrainFailure(*handler, absl::StatusCode::kInvalidArgument).ok());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
}

TEST_F(MemoryOrchestratorTest, DrainCompletedMidTermCompactionsPropagatesCompactorFailure) {
    auto compactor =
        std::make_shared<RecordingMidTermCompactor>(absl::InternalError("compaction failed"));
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    const absl::StatusOr<std::size_t> drained = handler->DrainCompletedMidTermCompactions();
    ASSERT_FALSE(drained.ok());
    EXPECT_EQ(drained.status().code(), absl::StatusCode::kInternal);

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
}

TEST_F(MemoryOrchestratorTest, AsyncDrainPropagatesMidTermPersistenceFailureWithoutMutatingState) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->upsert_episode_status = absl::InternalError("episode write failed");
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();
    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:55Z")).ok());

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    ASSERT_TRUE(compactor->WaitForRequestCount(1U));

    const absl::Status status = WaitForDrainFailure(*handler, absl::StatusCode::kInternal);
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_TRUE(store->stub_writes.empty());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::OngoingEpisode);
    ASSERT_TRUE(state.conversation.items[0].ongoing_episode.has_value());
    ASSERT_EQ(state.conversation.items[0].ongoing_episode->messages.size(), 2U);
}

} // namespace
} // namespace isla::server::memory
