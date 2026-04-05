#include "isla/server/memory/memory_orchestrator.hpp"

#include <optional>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "server/memory/src/long_term/sleep_cycle_extraction_builder.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

} // namespace

absl::StatusOr<bool> MemoryOrchestrator::FlushLiveTailForSleepCycle() {
    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return false;
    }

    const std::size_t tail_index = conversation.items.size() - 1U;
    const ConversationItem& tail_item = conversation.items.back();
    if (tail_item.type != ConversationItemType::OngoingEpisode) {
        return false;
    }
    if (!tail_item.ongoing_episode.has_value() || tail_item.ongoing_episode->messages.empty()) {
        return false;
    }
    if (mid_term_compactor_ == nullptr) {
        return absl::FailedPreconditionError(
            "sleep cycle requires a mid-term compactor to flush the live conversation tail");
    }

    absl::StatusOr<OngoingEpisodeFlushCandidate> candidate =
        memory_.CaptureOngoingEpisodeForFlush(tail_index);
    if (!candidate.ok()) {
        return candidate.status();
    }

    absl::StatusOr<CompletedFlushBuildInput> build_input = CompactFlushCandidate(
        mid_term_compactor_, session_id_, *candidate, "synchronous sleep-cycle");
    if (!build_input.ok()) {
        return build_input.status();
    }
    const CompletedOngoingEpisodeFlush completed_flush = BuildCompletedEpisodeFlush(
        candidate->conversation_item_index, std::move(*build_input), std::nullopt);

    if (absl::Status status = ApplyCompletedEpisodeFlush(completed_flush); !status.ok()) {
        return status;
    }

    VLOG(1) << "MemoryOrchestrator synchronously flushed live tail for sleep cycle session_id="
            << SanitizeForLog(session_id_) << " conversation_item_index=" << tail_index
            << " episode_id=" << SanitizeForLog(completed_flush.episode.episode_id)
            << " message_count=" << candidate->ongoing_episode.messages.size();
    return true;
}

absl::StatusOr<std::size_t>
MemoryOrchestrator::ConsolidateToLongTerm(const std::vector<Episode>& mid_term_episodes) {
    if (store_ == nullptr || mid_term_episodes.empty()) {
        return 0;
    }

    const std::string& user_id = memory_.snapshot().conversation.user_id;
    // Mirror the retrieval-path gating in memory_orchestrator_session.cpp: only advertise the
    // embedding client to the builder when a model is actually configured. The builder also
    // defends against this internally, but gating here keeps the "edge embeddings disabled"
    // contract visible at the orchestrator layer.
    const bool edge_embeddings_enabled =
        retrieval_embedding_client_ != nullptr && !retrieval_embedding_model_.empty();
    absl::StatusOr<SleepCycleExtractionResult> extraction =
        BuildSleepCycleExtractionResult(SleepCycleExtractionBuilderInput{
            .session_id = session_id_,
            .user_id = user_id,
            .mid_term_episodes = &mid_term_episodes,
            .store = store_.get(),
            .semantic_extractor = sleep_cycle_semantic_extractor_.get(),
            .embedding_client =
                edge_embeddings_enabled ? retrieval_embedding_client_.get() : nullptr,
            .embedding_model = edge_embeddings_enabled
                                   ? std::string_view(retrieval_embedding_model_)
                                   : std::string_view{},
        });
    if (!extraction.ok()) {
        LOG(WARNING) << "MemoryOrchestrator failed to build sleep-cycle extraction result"
                     << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                     << SanitizeForLog(extraction.status().message()) << "'";
        return extraction.status();
    }

    if (absl::Status status = store_->PersistSleepCycleExtraction(*extraction); !status.ok()) {
        LOG(WARNING) << "MemoryOrchestrator failed to persist sleep-cycle extraction batch"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " entity_count=" << extraction->entities.size()
                     << " relationship_count=" << extraction->relationships.size()
                     << " long_term_episode_count=" << extraction->long_term_episodes.size()
                     << " episode_link_count=" << extraction->long_term_episode_entity_links.size()
                     << " detail='" << SanitizeForLog(status.message()) << "'";
        return status;
    }

    LOG(INFO) << "MemoryOrchestrator consolidated " << extraction->long_term_episodes.size()
              << " mid-term episodes to long-term storage"
              << " session_id=" << SanitizeForLog(session_id_);
    return extraction->long_term_episodes.size();
}

absl::StatusOr<SleepCycleResult> MemoryOrchestrator::RunSleepCycle(Timestamp cycle_time) {
    if (absl::Status session_status = ValidateSessionReadyForPersistence(); !session_status.ok()) {
        return session_status;
    }

    const absl::StatusOr<std::size_t> drained_pending_compactions =
        AwaitAndDrainAllPendingMidTermCompactions();
    if (!drained_pending_compactions.ok()) {
        return drained_pending_compactions.status();
    }

    const absl::StatusOr<bool> flushed_live_tail = FlushLiveTailForSleepCycle();
    if (!flushed_live_tail.ok()) {
        return flushed_live_tail.status();
    }

    const WorkingMemoryState& state_before_clear = memory_.snapshot();
    const absl::StatusOr<std::size_t> consolidated =
        ConsolidateToLongTerm(state_before_clear.mid_term_episodes);
    if (!consolidated.ok()) {
        return consolidated.status();
    }

    SleepCycleResult result{
        .drained_pending_mid_term_compactions = *drained_pending_compactions,
        .synchronously_flushed_live_episodes = *flushed_live_tail ? 1U : 0U,
        .consolidated_long_term_episode_count = *consolidated,
        .cleared_mid_term_episode_count = state_before_clear.mid_term_episodes.size(),
        .cleared_conversation_item_count = state_before_clear.conversation.items.size(),
    };

    WorkingMemoryState cleared_state = state_before_clear;
    cleared_state.mid_term_episodes.clear();
    cleared_state.retrieved_memory.reset();
    cleared_state.conversation.items.clear();

    if (absl::Status status = PersistUserWorkingMemorySnapshot(cleared_state, cycle_time);
        !status.ok()) {
        return status;
    }

    if (store_ != nullptr) {
        if (absl::Status status = store_->ClearSessionWorkingSet(session_id_); !status.ok()) {
            LOG(WARNING) << "MemoryOrchestrator store.ClearSessionWorkingSet failed during sleep "
                            "cycle"
                         << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(status.message()) << "'";
            return status;
        }
    }

    memory_.ClearForSleepCycle();

    LOG(INFO)
        << "MemoryOrchestrator completed sleep cycle session_id=" << SanitizeForLog(session_id_)
        << " drained_pending_mid_term_compactions=" << result.drained_pending_mid_term_compactions
        << " synchronously_flushed_live_episodes=" << result.synchronously_flushed_live_episodes
        << " consolidated_long_term_episode_count=" << result.consolidated_long_term_episode_count
        << " cleared_mid_term_episode_count=" << result.cleared_mid_term_episode_count
        << " cleared_conversation_item_count=" << result.cleared_conversation_item_count;
    return result;
}

} // namespace isla::server::memory
