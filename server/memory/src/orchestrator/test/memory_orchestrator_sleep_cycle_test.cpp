#include "memory_orchestrator_test_support.hpp"

namespace isla::server::memory {
namespace {

TEST_F(MemoryOrchestratorTest, RunSleepCycleDrainsPendingCompactionsAndClearsTransientState) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();
    handler->mutable_memory().UpsertActiveModel("entity_user", "Airi, the user.");

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    handler->mutable_memory().SetRetrievedMemory("recent retrieval");
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    EXPECT_TRUE(handler->HasPendingMidTermCompactions());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->drained_pending_mid_term_compactions, 1U);
    EXPECT_EQ(result->synchronously_flushed_live_episodes, 0U);
    EXPECT_EQ(result->cleared_mid_term_episode_count, 1U);
    EXPECT_EQ(result->cleared_conversation_item_count, 1U);
    EXPECT_FALSE(handler->HasPendingMidTermCompactions());

    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_TRUE(state.mid_term_episodes.empty());
    EXPECT_FALSE(state.retrieved_memory.has_value());
    EXPECT_TRUE(state.conversation.items.empty());
    ASSERT_EQ(state.system_prompt.persistent_memory_cache.active_models.size(), 1U);
    EXPECT_EQ(state.system_prompt.persistent_memory_cache.active_models.front().entity_id,
              "entity_user");
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleClearsPersistedWorkingSetAndSnapshot) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->cleared_session_ids.size(), 1U);
    EXPECT_EQ(store->cleared_session_ids.front(), "srv_test");
    ASSERT_FALSE(store->user_working_memory_records.empty());
    const UserWorkingMemoryRecord& latest_snapshot = store->user_working_memory_records.back();
    EXPECT_EQ(latest_snapshot.updated_at, Ts("2026-03-09T04:00:00Z"));
    EXPECT_TRUE(latest_snapshot.working_memory.mid_term_episodes.empty());
    EXPECT_FALSE(latest_snapshot.working_memory.retrieved_memory.has_value());
    EXPECT_TRUE(latest_snapshot.working_memory.conversation.items.empty());
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSynchronouslyFlushesRemainingLiveTail) {
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->drained_pending_mid_term_compactions, 0U);
    EXPECT_EQ(result->synchronously_flushed_live_episodes, 1U);
    EXPECT_EQ(result->cleared_mid_term_episode_count, 1U);
    EXPECT_EQ(result->cleared_conversation_item_count, 1U);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSynchronouslyFlushesRemainingLiveTailIntoStore) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->synchronously_flushed_live_episodes, 1U);
    ASSERT_EQ(store->episode_writes.size(), 1U);
    EXPECT_EQ(store->episode_writes.front().session_id, "srv_test");
    ASSERT_EQ(store->stub_writes.size(), 1U);
    EXPECT_EQ(store->stub_writes.front().session_id, "srv_test");
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleRejectsLiveTailWhenNoCompactorConfigured) {
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandler();
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleDoesNotMutateLiveOrPersistedWorkingSetWhenSnapshotPersistenceFails) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    store->upsert_user_working_memory_status = absl::InternalError("snapshot write failed");
    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    EXPECT_TRUE(store->cleared_session_ids.empty());

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleDoesNotMutateLiveWorkingSetWhenClearingPersistedTablesFails) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    store->clear_working_set_status = absl::InternalError("clear working set failed");
    const std::size_t snapshot_count_before_sleep = store->user_working_memory_records.size();
    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);
    ASSERT_GE(store->user_working_memory_records.size(), snapshot_count_before_sleep + 1U);
    const UserWorkingMemoryRecord& latest_snapshot = store->user_working_memory_records.back();
    EXPECT_TRUE(latest_snapshot.working_memory.mid_term_episodes.empty());
    EXPECT_TRUE(latest_snapshot.working_memory.conversation.items.empty());

    const WorkingMemoryState& state = handler->memory().snapshot();
    ASSERT_EQ(state.mid_term_episodes.size(), 1U);
    ASSERT_EQ(state.conversation.items.size(), 1U);
    EXPECT_EQ(state.conversation.items[0].type, ConversationItemType::EpisodeStub);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleConsolidatesMidTermEpisodesToLongTerm) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());
    EXPECT_TRUE(handler->HasPendingMidTermCompactions());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->consolidated_long_term_episode_count, 1U);
    ASSERT_EQ(store->extraction_results.size(), 1U);
    ASSERT_EQ(store->long_term_episode_writes.size(), 1U);

    const LongTermEpisode& lte = store->long_term_episode_writes.front().episode;
    EXPECT_EQ(lte.lte_id, "lte_" + lte.original_episode_ids.front());
    EXPECT_EQ(lte.user_id, "user_001");
    EXPECT_EQ(lte.summary_compressed, "summary");
    EXPECT_EQ(lte.keywords, std::vector<std::string>{ "memory" });
    EXPECT_EQ(lte.original_episode_ids.size(), 1U);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleAbortsWhenConsolidationFails) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>();
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    store->persist_sleep_cycle_extraction_status =
        absl::InternalError("sleep-cycle extraction persistence failed");
    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInternal);

    // Mid-term data is preserved — nothing was cleared.
    EXPECT_TRUE(store->cleared_session_ids.empty());
    const WorkingMemoryState& state = handler->memory().snapshot();
    EXPECT_EQ(state.mid_term_episodes.size(), 1U);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSkipsConsolidationWithoutStore) {
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

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->consolidated_long_term_episode_count, 0U);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleConsolidationMapsEmptyEmbeddingToNullopt) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "memory" },
        .salience = kExpandableEpisodeSalienceThreshold,
        .embedding = {},
    });
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->extraction_results.size(), 1U);
    ASSERT_EQ(store->long_term_episode_writes.size(), 1U);
    EXPECT_FALSE(store->long_term_episode_writes.front().episode.embedding.has_value());
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleUsesStructuredExtractionBatchPlaceholder) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "renderer" },
        .salience = 6,
        .embedding = {},
    });
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(compactor, store);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->extraction_results.size(), 1U);
    EXPECT_TRUE(store->entity_writes.empty());
    EXPECT_TRUE(store->relationship_writes.empty());
    ASSERT_EQ(store->long_term_episode_writes.size(), 1U);
    EXPECT_EQ(store->long_term_episode_writes.front().episode.lte_id, "lte_ep_srv_test_1");
    EXPECT_EQ(store->long_term_episode_writes.front().episode.original_episode_ids,
              std::vector<std::string>({ "ep_srv_test_1" }));
    EXPECT_TRUE(store->long_term_episode_entity_links.empty());
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleSemanticExtractionCreatesEntitiesRelationshipsAndLinks) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "mochi" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor =
        std::make_shared<RecordingSleepCycleSemanticExtractor>(SleepCycleSemanticExtractionResult{
            .entities =
                {
                    SemanticEntityCandidate{
                        .label = "Mochi",
                        .category = "pet",
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
            .relationships =
                {
                    SemanticRelationshipObservation{
                        .from_label = "user",
                        .from_category = "person",
                        .predicate = "owns",
                        .to_label = "Mochi",
                        .to_category = "pet",
                        .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
        });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(semantic_extractor->requests().size(), 1U);
    ASSERT_EQ(store->extraction_results.size(), 1U);
    ASSERT_EQ(store->entity_writes.size(), 2U);
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    ASSERT_EQ(store->long_term_episode_entity_links.size(), 1U);

    const auto user_entity_it = std::find_if(
        store->entity_writes.begin(), store->entity_writes.end(),
        [](const EntityWrite& write) { return write.entity.entity_id == kUserEntityId; });
    ASSERT_NE(user_entity_it, store->entity_writes.end());
    EXPECT_EQ(user_entity_it->entity.activeness, 3);

    const auto mochi_entity_it =
        std::find_if(store->entity_writes.begin(), store->entity_writes.end(),
                     [](const EntityWrite& write) { return write.entity.label == "Mochi"; });
    ASSERT_NE(mochi_entity_it, store->entity_writes.end());
    EXPECT_EQ(mochi_entity_it->entity.category, "pet");
    EXPECT_EQ(mochi_entity_it->entity.activeness, 3);

    const Relationship& relationship = store->relationship_writes.front().relationship;
    EXPECT_EQ(relationship.from_entity_id, kUserEntityId);
    EXPECT_EQ(relationship.to_entity_id, mochi_entity_it->entity.entity_id);
    EXPECT_EQ(relationship.predicate, "owns");
    EXPECT_DOUBLE_EQ(relationship.weight, 10.0);
    EXPECT_EQ(relationship.observation_count, 1);
    EXPECT_EQ(relationship.source_episode_ids, std::vector<std::string>({ "ep_srv_test_1" }));

    const LongTermEpisodeEntityLink& link = store->long_term_episode_entity_links.front();
    EXPECT_EQ(link.lte_id, "lte_ep_srv_test_1");
    EXPECT_THAT(link.entity_ids,
                ::testing::UnorderedElementsAre(std::string(kUserEntityId),
                                                mochi_entity_it->entity.entity_id));
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSemanticExtractionStrengthensExistingRelationship) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 5,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_existing",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "trusts",
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = 10.0,
            .observation_count = 1,
            .last_observed_at = Ts("2026-03-01T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "trust" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "trusts",
                                .to_label = "assistant",
                                .to_category = "assistant",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::StrongInference,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        EXPECT_THAT(request.existing_relationships,
                    ::testing::ElementsAre(::testing::Field(
                        &ExistingSemanticRelationship::relationship_id, "rel_existing")));
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "trusts",
                            .to_label = "assistant",
                            .to_category = "assistant",
                            .operation = SemanticRelationshipOperation::Strengthen,
                            .target_relationship_id = std::string("rel_existing"),
                            .evidence = SemanticRelationshipEvidence::StrongInference,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(semantic_extractor->requests().size(), 2U);
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    const Relationship& relationship = store->relationship_writes.front().relationship;
    EXPECT_EQ(relationship.relationship_id, "rel_existing");
    EXPECT_EQ(relationship.from_entity_id, kUserEntityId);
    EXPECT_EQ(relationship.to_entity_id, kAssistantEntityId);
    EXPECT_DOUBLE_EQ(relationship.weight, 14.0);
    EXPECT_EQ(relationship.observation_count, 2);
    EXPECT_EQ(relationship.source_episode_ids,
              std::vector<std::string>({ "ep_old", "ep_srv_test_1" }));
    EXPECT_EQ(relationship.created_at, Ts("2026-03-01T14:00:00Z"));

    const auto user_entity_it = std::find_if(
        store->entity_writes.begin(), store->entity_writes.end(),
        [](const EntityWrite& write) { return write.entity.entity_id == kUserEntityId; });
    ASSERT_NE(user_entity_it, store->entity_writes.end());
    EXPECT_EQ(user_entity_it->entity.activeness, 5);
    EXPECT_EQ(user_entity_it->entity.created_at, Ts("2026-03-01T14:00:00Z"));
    EXPECT_EQ(user_entity_it->entity.updated_at, Ts("2026-03-08T14:00:01Z"));
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleSemanticExtractionTruncatesOversizedRelationshipContextDeterministically) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    for (int i = 0; i < 300; ++i) {
        const std::string padded_index =
            i < 10 ? "00" + std::to_string(i)
                   : (i < 100 ? "0" + std::to_string(i) : std::to_string(i));
        store->relationships.push_back(Relationship{
            .relationship_id = "rel_" + padded_index,
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "context_predicate_" + padded_index,
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = static_cast<double>(i + 1),
            .observation_count = 1,
            .last_observed_at = Ts("2026-03-08T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        });
    }
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "context" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "trusts",
                            .to_label = "assistant",
                            .to_category = "assistant",
                            .operation = SemanticRelationshipOperation::Append,
                            .target_relationship_id = std::nullopt,
                            .evidence = SemanticRelationshipEvidence::WeakInference,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
        }
        EXPECT_EQ(request.existing_relationships.size(), 256U);
        EXPECT_EQ(request.existing_relationships.front().relationship_id, "rel_299");
        EXPECT_EQ(request.existing_relationships.back().relationship_id, "rel_044");
        return SleepCycleSemanticExtractionResult{};
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(semantic_extractor->requests().size(), 2U);
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSemanticExtractionSupersedesExistingRelationship) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = "entity_portland",
            .user_id = "user_001",
            .label = "Portland",
            .category = "location",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_city_old",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "lives_in",
            .to_entity_id = "entity_portland",
            .weight = 5.0,
            .observation_count = 1,
            .last_observed_at = Ts("2026-03-01T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "move" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "lives_in",
                                .to_label = "Seattle",
                                .to_category = "location",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        EXPECT_THAT(request.existing_relationships,
                    ::testing::ElementsAre(::testing::Field(
                        &ExistingSemanticRelationship::relationship_id, "rel_city_old")));
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "lives_in",
                            .to_label = "Seattle",
                            .to_category = "location",
                            .operation = SemanticRelationshipOperation::Supersede,
                            .target_relationship_id = std::string("rel_city_old"),
                            .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(semantic_extractor->requests().size(), 2U);
    ASSERT_EQ(store->relationship_writes.size(), 2U);

    const auto archived_relationship_it =
        std::find_if(store->relationship_writes.begin(), store->relationship_writes.end(),
                     [](const RelationshipWrite& write) {
                         return write.relationship.relationship_id == "rel_city_old";
                     });
    ASSERT_NE(archived_relationship_it, store->relationship_writes.end());
    EXPECT_TRUE(archived_relationship_it->relationship.is_archived);
    EXPECT_EQ(archived_relationship_it->relationship.archived_at, Ts("2026-03-08T14:00:01Z"));
    ASSERT_TRUE(archived_relationship_it->relationship.superseded_by.has_value());

    const auto new_relationship_it =
        std::find_if(store->relationship_writes.begin(), store->relationship_writes.end(),
                     [](const RelationshipWrite& write) {
                         return write.relationship.relationship_id != "rel_city_old";
                     });
    ASSERT_NE(new_relationship_it, store->relationship_writes.end());
    EXPECT_EQ(new_relationship_it->relationship.from_entity_id, kUserEntityId);
    EXPECT_EQ(new_relationship_it->relationship.predicate, "lives_in");
    EXPECT_DOUBLE_EQ(new_relationship_it->relationship.weight, 10.0);
    EXPECT_EQ(new_relationship_it->relationship.observation_count, 1);
    EXPECT_EQ(archived_relationship_it->relationship.superseded_by,
              std::optional<std::string>(new_relationship_it->relationship.relationship_id));
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleSemanticExtractionDamagesOldRelationshipWhenSupersedeLoses) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = "entity_portland",
            .user_id = "user_001",
            .label = "Portland",
            .category = "location",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_city_old",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "lives_in",
            .to_entity_id = "entity_portland",
            .weight = 10.0,
            .observation_count = 2,
            .last_observed_at = Ts("2026-03-08T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "move" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "lives_in",
                                .to_label = "Seattle",
                                .to_category = "location",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::WeakInference,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "lives_in",
                            .to_label = "Seattle",
                            .to_category = "location",
                            .operation = SemanticRelationshipOperation::Supersede,
                            .target_relationship_id = std::string("rel_city_old"),
                            .evidence = SemanticRelationshipEvidence::WeakInference,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    const Relationship& relationship = store->relationship_writes.front().relationship;
    EXPECT_EQ(relationship.relationship_id, "rel_city_old");
    EXPECT_FALSE(relationship.is_archived);
    EXPECT_DOUBLE_EQ(relationship.weight, 9.0);
    EXPECT_EQ(relationship.observation_count, 2);
    EXPECT_EQ(relationship.source_episode_ids, std::vector<std::string>({ "ep_old" }));
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleSemanticExtractionRejectsSupersedeForSameRelationshipTriple) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_city_old",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "trusts",
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = 10.0,
            .observation_count = 2,
            .last_observed_at = Ts("2026-03-08T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "move" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "trusts",
                                .to_label = "assistant",
                                .to_category = "assistant",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::WeakInference,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "trusts",
                            .to_label = "assistant",
                            .to_category = "assistant",
                            .operation = SemanticRelationshipOperation::Supersede,
                            .target_relationship_id = std::string("rel_city_old"),
                            .evidence = SemanticRelationshipEvidence::WeakInference,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(), ::testing::HasSubstr("SUPERSEDE target must differ"));
}

TEST_F(MemoryOrchestratorTest,
       RunSleepCycleSemanticExtractionRejectsSupersedeForUnrelatedTargetRelationship) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_existing",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "trusts",
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = 10.0,
            .observation_count = 2,
            .last_observed_at = Ts("2026-03-08T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "move" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "lives_in",
                                .to_label = "Seattle",
                                .to_category = "location",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "lives_in",
                            .to_label = "Seattle",
                            .to_category = "location",
                            .operation = SemanticRelationshipOperation::Supersede,
                            .target_relationship_id = std::string("rel_existing"),
                            .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(), ::testing::HasSubstr("must be comparable"));
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSemanticExtractionRejectsArchivedTargetRelationship) {
    auto store = std::make_shared<RecordingMemoryStore>();
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = "entity_portland",
            .user_id = "user_001",
            .label = "Portland",
            .category = "location",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 3,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_archived",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "lives_in",
            .to_entity_id = "entity_portland",
            .weight = 10.0,
            .observation_count = 2,
            .last_observed_at = Ts("2026-03-08T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .is_archived = true,
            .archived_at = Ts("2026-03-08T14:30:00Z"),
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
        Relationship{
            .relationship_id = "rel_context",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "trusts",
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = 4.0,
            .observation_count = 1,
            .last_observed_at = Ts("2026-03-07T14:00:00Z"),
            .source_episode_ids = { "ep_context" },
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "move" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                    .relationships =
                        {
                            SemanticRelationshipObservation{
                                .from_label = "user",
                                .from_category = "person",
                                .predicate = "lives_in",
                                .to_label = "Seattle",
                                .to_category = "location",
                                .operation = SemanticRelationshipOperation::Append,
                                .target_relationship_id = std::nullopt,
                                .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                                .source_episode_ids = { "ep_srv_test_1" },
                            },
                        },
                };
        }
        return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "lives_in",
                            .to_label = "Seattle",
                            .to_category = "location",
                            .operation = SemanticRelationshipOperation::Supersede,
                            .target_relationship_id = std::string("rel_archived"),
                            .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
    });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(), ::testing::HasSubstr("archived target relationship"));
}

TEST_F(MemoryOrchestratorTest, RunSleepCyclePopulatesRelationshipEmbeddingsForNewRelationships) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "mochi" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor =
        std::make_shared<RecordingSleepCycleSemanticExtractor>(SleepCycleSemanticExtractionResult{
            .relationships =
                {
                    SemanticRelationshipObservation{
                        .from_label = "user",
                        .from_category = "person",
                        .predicate = "owns",
                        .to_label = "Mochi",
                        .to_category = "pet",
                        .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
        });
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();
    // Retrieval-path embedding for HandleUserQuery + relationship-edge embedding for the sleep
    // cycle. The second call is the one under test.
    EXPECT_CALL(*embedding_client, Embed(_))
        .WillOnce(Return(Embedding(kEmbeddingDimensions, 0.1)))
        .WillOnce(Return(Embedding(kEmbeddingDimensions, 0.42)));
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(
        compactor, store, nullptr, semantic_extractor,
        kImmediateMidTermFlushDeciderIntervalUserTurns, nullptr,
        kDefaultRetrievedMemoryRerankerMinScore, embedding_client, "test-embedding-model");
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    const Relationship& relationship = store->relationship_writes.front().relationship;
    ASSERT_TRUE(relationship.embedding.has_value());
    EXPECT_EQ(relationship.embedding->size(), kEmbeddingDimensions);
    EXPECT_DOUBLE_EQ(relationship.embedding->front(), 0.42);
}

TEST_F(MemoryOrchestratorTest, RunSleepCyclePreservesExistingRelationshipEmbeddingOnStrengthen) {
    auto store = std::make_shared<RecordingMemoryStore>();
    const Embedding preserved_embedding(kEmbeddingDimensions, 0.77);
    store->entities = {
        Entity{
            .entity_id = std::string(kUserEntityId),
            .user_id = "user_001",
            .label = "user",
            .category = "person",
            .activeness = 4,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
        Entity{
            .entity_id = std::string(kAssistantEntityId),
            .user_id = "user_001",
            .label = "assistant",
            .category = "assistant",
            .activeness = 5,
            .created_at = Ts("2026-03-01T14:00:00Z"),
            .updated_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    store->relationships = {
        Relationship{
            .relationship_id = "rel_existing",
            .user_id = "user_001",
            .from_entity_id = std::string(kUserEntityId),
            .predicate = "trusts",
            .to_entity_id = std::string(kAssistantEntityId),
            .weight = 10.0,
            .observation_count = 1,
            .last_observed_at = Ts("2026-03-01T14:00:00Z"),
            .source_episode_ids = { "ep_old" },
            .embedding = preserved_embedding,
            .created_at = Ts("2026-03-01T14:00:00Z"),
        },
    };
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "trust" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor = std::make_shared<RecordingSleepCycleSemanticExtractor>();
    semantic_extractor->SetExtractFn([](const SleepCycleSemanticExtractionRequest& request)
                                         -> absl::StatusOr<SleepCycleSemanticExtractionResult> {
        if (request.existing_relationships.empty()) {
            return SleepCycleSemanticExtractionResult{
                .relationships =
                    {
                        SemanticRelationshipObservation{
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "trusts",
                            .to_label = "assistant",
                            .to_category = "assistant",
                            .operation = SemanticRelationshipOperation::Append,
                            .evidence = SemanticRelationshipEvidence::StrongInference,
                            .source_episode_ids = { "ep_srv_test_1" },
                        },
                    },
            };
        }
        return SleepCycleSemanticExtractionResult{
            .relationships =
                {
                    SemanticRelationshipObservation{
                        .from_label = "user",
                        .from_category = "person",
                        .predicate = "trusts",
                        .to_label = "assistant",
                        .to_category = "assistant",
                        .operation = SemanticRelationshipOperation::Strengthen,
                        .target_relationship_id = std::string("rel_existing"),
                        .evidence = SemanticRelationshipEvidence::StrongInference,
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
        };
    });
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();
    // Retrieval path embeds the user query once; the builder must NOT re-embed the strengthened
    // edge because it already has an embedding.
    EXPECT_CALL(*embedding_client, Embed(_)).WillOnce(Return(Embedding(kEmbeddingDimensions, 0.1)));
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(
        compactor, store, nullptr, semantic_extractor,
        kImmediateMidTermFlushDeciderIntervalUserTurns, nullptr,
        kDefaultRetrievedMemoryRerankerMinScore, embedding_client, "test-embedding-model");
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    const Relationship& relationship = store->relationship_writes.front().relationship;
    ASSERT_TRUE(relationship.embedding.has_value());
    EXPECT_EQ(*relationship.embedding, preserved_embedding);
}

TEST_F(MemoryOrchestratorTest, RunSleepCyclePersistsRelationshipsWhenEmbeddingClientFails) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "mochi" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor =
        std::make_shared<RecordingSleepCycleSemanticExtractor>(SleepCycleSemanticExtractionResult{
            .relationships =
                {
                    SemanticRelationshipObservation{
                        .from_label = "user",
                        .from_category = "person",
                        .predicate = "owns",
                        .to_label = "Mochi",
                        .to_category = "pet",
                        .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
        });
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();
    EXPECT_CALL(*embedding_client, Embed(_))
        .WillOnce(Return(Embedding(kEmbeddingDimensions, 0.1)))
        .WillOnce(Return(absl::InternalError("embedding service unavailable")));
    absl::StatusOr<MemoryOrchestrator> handler = MakeHandlerWithCompactor(
        compactor, store, nullptr, semantic_extractor,
        kImmediateMidTermFlushDeciderIntervalUserTurns, nullptr,
        kDefaultRetrievedMemoryRerankerMinScore, embedding_client, "test-embedding-model");
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    // Embedding failures must not fail the sleep cycle — the relationship is persisted
    // without an embedding and will be lazily backfilled on a later consolidation.
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    const Relationship& relationship = store->relationship_writes.front().relationship;
    EXPECT_FALSE(relationship.embedding.has_value());
}

TEST_F(MemoryOrchestratorTest, RunSleepCycleSkipsRelationshipEmbeddingsWhenNoEmbeddingClient) {
    auto store = std::make_shared<RecordingMemoryStore>();
    auto compactor = std::make_shared<RecordingMidTermCompactor>(CompactedMidTermEpisode{
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "mochi" },
        .salience = 6,
        .embedding = {},
    });
    auto semantic_extractor =
        std::make_shared<RecordingSleepCycleSemanticExtractor>(SleepCycleSemanticExtractionResult{
            .relationships =
                {
                    SemanticRelationshipObservation{
                        .from_label = "user",
                        .from_category = "person",
                        .predicate = "owns",
                        .to_label = "Mochi",
                        .to_category = "pet",
                        .evidence = SemanticRelationshipEvidence::ExplicitStatement,
                        .source_episode_ids = { "ep_srv_test_1" },
                    },
                },
        });
    absl::StatusOr<MemoryOrchestrator> handler =
        MakeHandlerWithCompactor(compactor, store, nullptr, semantic_extractor);
    ASSERT_TRUE(handler.ok()) << handler.status();

    ASSERT_TRUE(handler->BeginSession(Ts("2026-03-08T13:59:59Z")).ok());
    ASSERT_TRUE(handler
                    ->HandleUserQuery(GatewayUserQuery("srv_test", "turn_001", "hello",
                                                       Ts("2026-03-08T14:00:00Z")))
                    .ok());
    ASSERT_TRUE(handler
                    ->HandleAssistantReply(GatewayAssistantReply("srv_test", "turn_001", "hi there",
                                                                 Ts("2026-03-08T14:00:01Z")))
                    .ok());

    const absl::StatusOr<SleepCycleResult> result =
        handler->RunSleepCycle(Ts("2026-03-09T04:00:00Z"));

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(store->relationship_writes.size(), 1U);
    EXPECT_FALSE(store->relationship_writes.front().relationship.embedding.has_value());
}

} // namespace
} // namespace isla::server::memory
