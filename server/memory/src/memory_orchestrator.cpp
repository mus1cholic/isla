#include "isla/server/memory/memory_orchestrator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/memory/conversation.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

Timestamp NowTimestamp() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

absl::Status ValidateSplitFlushTarget(const Conversation& conversation,
                                      std::size_t conversation_item_index,
                                      std::size_t split_at_message_index) {
    if (conversation_item_index >= conversation.items.size()) {
        return invalid_argument("split flush target exceeds conversation size");
    }
    const auto& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return invalid_argument("split flush target must be an ongoing episode");
    }
    return ValidateOngoingEpisodeBoundaryPlan(
        *item.ongoing_episode, OngoingEpisodeBoundaryPlan{
                                   .boundary_message_indices = { split_at_message_index },
                                   .tail_complete = false,
                               });
}

absl::StatusOr<std::optional<std::size_t>>
FindLiveConversationItemIndex(const Conversation& conversation,
                              const MidTermFlushDecision& decision, std::string_view session_id) {
    if (conversation.items.empty()) {
        if (!decision.boundaries.empty() || decision.tail_complete) {
            LOG(WARNING)
                << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                << " session_id=" << SanitizeForLog(session_id)
                << " detail='flush decider returned completed segments for an empty conversation'";
            return invalid_argument("mid-term flush decider cannot return completed segments for "
                                    "an empty conversation");
        }
        return std::nullopt;
    }

    if (decision.boundaries.empty() && !decision.tail_complete) {
        VLOG(1) << "MemoryOrchestrator flush decider chose not to flush session_id="
                << SanitizeForLog(session_id);
        return std::nullopt;
    }

    const std::size_t item_index = conversation.items.size() - 1U;
    const ConversationItem& item = conversation.items[item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                     << " session_id=" << SanitizeForLog(session_id)
                     << " detail='final conversation item is not an ongoing episode'";
        return invalid_argument(
            "mid-term flush decider requires the final conversation item to be an ongoing episode");
    }

    OngoingEpisodeBoundaryPlan boundary_plan{
        .boundary_message_indices = {},
        .tail_complete = decision.tail_complete,
    };
    boundary_plan.boundary_message_indices.reserve(decision.boundaries.size());
    for (const MidTermFlushBoundary& boundary : decision.boundaries) {
        boundary_plan.boundary_message_indices.push_back(boundary.split_before_message_index);
    }
    if (absl::Status status =
            ValidateOngoingEpisodeBoundaryPlan(*item.ongoing_episode, boundary_plan);
        !status.ok()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid boundary plan from decider"
                     << " session_id=" << SanitizeForLog(session_id)
                     << " conversation_item_index=" << item_index << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return status;
    }

    VLOG(1) << "MemoryOrchestrator flush decider chose a conversation item for flush session_id="
            << SanitizeForLog(session_id) << " conversation_item_index=" << item_index
            << " boundary_count=" << decision.boundaries.size()
            << " tail_complete=" << (decision.tail_complete ? "true" : "false");
    return item_index;
}

absl::StatusOr<OngoingEpisodeFlushCandidate>
CaptureOngoingEpisodeForFlush(const Conversation& conversation,
                              std::size_t conversation_item_index) {
    if (conversation_item_index >= conversation.items.size()) {
        return invalid_argument("flush target exceeds conversation size");
    }
    const auto& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return invalid_argument("flush target must be an ongoing episode");
    }
    if (item.ongoing_episode->messages.empty()) {
        return invalid_argument("flush target must contain at least one message");
    }
    return OngoingEpisodeFlushCandidate{
        .conversation_item_index = conversation_item_index,
        .ongoing_episode = *item.ongoing_episode,
    };
}

absl::StatusOr<std::vector<OngoingEpisodeFlushCandidate>>
CaptureOngoingEpisodeSegmentsForFlush(const Conversation& conversation,
                                      std::size_t conversation_item_index,
                                      const MidTermFlushDecision& decision) {
    if (conversation_item_index >= conversation.items.size()) {
        return invalid_argument("flush target exceeds conversation size");
    }
    const ConversationItem& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return invalid_argument("flush target must be an ongoing episode");
    }

    OngoingEpisodeBoundaryPlan boundary_plan{
        .boundary_message_indices = {},
        .tail_complete = decision.tail_complete,
    };
    boundary_plan.boundary_message_indices.reserve(decision.boundaries.size());
    for (const MidTermFlushBoundary& boundary : decision.boundaries) {
        boundary_plan.boundary_message_indices.push_back(boundary.split_before_message_index);
    }

    absl::StatusOr<std::vector<OngoingEpisodeMessageRange>> ranges =
        BuildCompletedEpisodeRanges(*item.ongoing_episode, boundary_plan);
    if (!ranges.ok()) {
        return ranges.status();
    }

    std::vector<OngoingEpisodeFlushCandidate> candidates;
    candidates.reserve(ranges->size());
    for (const OngoingEpisodeMessageRange& range : *ranges) {
        absl::StatusOr<OngoingEpisode> segment = SliceOngoingEpisodeMessages(
            *item.ongoing_episode, range.begin_message_index, range.end_message_index);
        if (!segment.ok()) {
            return segment.status();
        }
        candidates.push_back(OngoingEpisodeFlushCandidate{
            .conversation_item_index = conversation_item_index,
            .ongoing_episode = std::move(*segment),
        });
    }
    return candidates;
}

} // namespace

MemoryOrchestrator::MemoryOrchestrator(std::string session_id, WorkingMemory memory,
                                       MemoryStorePtr store,
                                       MidTermFlushDeciderPtr mid_term_flush_decider,
                                       MidTermCompactorPtr mid_term_compactor,
                                       std::size_t mid_term_flush_decider_interval_user_turns)
    : session_id_(std::move(session_id)), memory_(std::move(memory)), store_(std::move(store)),
      mid_term_flush_decider_(std::move(mid_term_flush_decider)),
      mid_term_compactor_(std::move(mid_term_compactor)), pending_mid_term_flushes_(),
      mid_term_flush_decider_interval_user_turns_(mid_term_flush_decider_interval_user_turns),
      user_turns_since_last_mid_term_decider_run_(0), next_episode_sequence_(1),
      session_persisted_(false) {
    CHECK_GT(mid_term_flush_decider_interval_user_turns_, 0U)
        << "mid_term_flush_decider_interval_user_turns must be at least 1";
}

absl::StatusOr<MemoryOrchestrator> MemoryOrchestrator::Create(std::string session_id,
                                                              const MemoryOrchestratorInit& init) {
    if (session_id.empty()) {
        return invalid_argument("memory orchestrator must include a session_id");
    }
    if (init.user_id.empty()) {
        return invalid_argument("memory orchestrator must include a user_id");
    }
    if (init.mid_term_flush_decider_interval_user_turns == 0U) {
        return invalid_argument(
            "memory orchestrator mid-term flush decider interval must be at least 1 user turn");
    }

    absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
        .system_prompt = "",
        .user_id = init.user_id,
    });
    if (!memory.ok()) {
        return memory.status();
    }
    return MemoryOrchestrator(std::move(session_id), std::move(*memory), init.store,
                              init.mid_term_flush_decider, init.mid_term_compactor,
                              init.mid_term_flush_decider_interval_user_turns);
}

absl::Status MemoryOrchestrator::BeginSession(Timestamp create_time) {
    // Hydrate the persistent cache before persisting so the initial snapshot includes long-term
    // entity data, and hydration failure does not leave a persisted-but-uninitialized session.
    if (absl::Status status = HydratePersistentMemoryCache(); !status.ok()) {
        return status;
    }
    return PersistSessionIfNeeded(create_time);
}

absl::Status MemoryOrchestrator::HydratePersistentMemoryCache() {
    if (!store_) {
        return absl::OkStatus();
    }

    const std::string& user_id = memory_.snapshot().conversation.user_id;
    const absl::StatusOr<std::vector<Entity>> entities = store_->ListEntitiesByUser(user_id);
    if (!entities.ok()) {
        if (absl::IsUnimplemented(entities.status())) {
            return absl::OkStatus();
        }
        LOG(WARNING) << "MemoryOrchestrator failed to load entities for cache hydration"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " user_id=" << SanitizeForLog(user_id) << " detail='"
                     << SanitizeForLog(entities.status().message()) << "'";
        return entities.status();
    }

    for (const Entity& entity : *entities) {
        if (entity.active_model_text.has_value() && !entity.active_model_text->empty()) {
            memory_.UpsertActiveModel(entity.entity_id, *entity.active_model_text);
        } else if (entity.familiar_label_text.has_value() && !entity.familiar_label_text->empty()) {
            memory_.UpsertFamiliarLabel(entity.entity_id, *entity.familiar_label_text);
        }
    }

    LOG(INFO) << "MemoryOrchestrator hydrated persistent memory cache"
              << " session_id=" << SanitizeForLog(session_id_)
              << " entity_count=" << entities->size();

    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::ValidateTurnText(std::string_view session_id,
                                                  std::string_view turn_id,
                                                  std::string_view role_label) const {
    if (session_id.empty()) {
        return invalid_argument(std::string("gateway ") + std::string(role_label) +
                                " text must include a session_id");
    }
    if (turn_id.empty()) {
        return invalid_argument(std::string("gateway ") + std::string(role_label) +
                                " text must include a turn_id");
    }
    if (session_id != session_id_) {
        LOG(WARNING) << "MemoryOrchestrator rejected turn text for mismatched session"
                     << " expected_session_id=" << SanitizeForLog(session_id_)
                     << " received_session_id=" << SanitizeForLog(session_id);
        return invalid_argument("gateway turn text session_id does not match orchestrator session");
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::ValidateSessionReadyForPersistence() const {
    if (store_ != nullptr && !session_persisted_) {
        return absl::FailedPreconditionError(
            "memory orchestrator requires BeginSession before handling conversation messages");
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistSessionIfNeeded(Timestamp create_time) {
    if (!store_ || session_persisted_) {
        return absl::OkStatus();
    }

    const WorkingMemoryState& state = memory_.snapshot();
    const MemorySessionRecord session_record{
        .session_id = session_id_,
        .user_id = state.conversation.user_id,
        .system_prompt = state.system_prompt.base_instructions,
        .created_at = create_time,
        .ended_at = std::nullopt,
    };
    if (absl::Status status = ValidateMemorySessionRecord(session_record); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->UpsertSession(session_record); !status.ok()) {
        LOG(WARNING)
            << "MemoryOrchestrator store.UpsertSession failed while persisting the session record"
            << " session_id=" << SanitizeForLog(session_id_)
            << " user_id=" << SanitizeForLog(state.conversation.user_id)
            << " session_created_at=" << SanitizeForLog(FormatTimestamp(create_time)) << " detail='"
            << SanitizeForLog(status.message()) << "'";
        return status;
    }
    if (absl::Status status = PersistUserWorkingMemorySnapshot(create_time); !status.ok()) {
        return status;
    }
    session_persisted_ = true;
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistUserWorkingMemorySnapshot(Timestamp updated_at) {
    return PersistUserWorkingMemorySnapshot(memory_.snapshot(), updated_at);
}

absl::Status MemoryOrchestrator::PersistUserWorkingMemorySnapshot(const WorkingMemoryState& state,
                                                                  Timestamp updated_at) {
    if (!store_) {
        return absl::OkStatus();
    }

    absl::StatusOr<std::string> rendered_working_memory = RenderWorkingMemoryPrompt(state);
    if (!rendered_working_memory.ok()) {
        return rendered_working_memory.status();
    }

    const UserWorkingMemoryRecord record{
        .user_id = state.conversation.user_id,
        .session_id = session_id_,
        .working_memory = state,
        .rendered_working_memory = std::move(*rendered_working_memory),
        .updated_at = updated_at,
    };
    if (absl::Status status = ValidateUserWorkingMemoryRecord(record); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->UpsertUserWorkingMemory(record); !status.ok()) {
        LOG(WARNING) << "MemoryOrchestrator store.UpsertUserWorkingMemory failed while "
                        "persisting the current user working-memory snapshot"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " user_id=" << SanitizeForLog(state.conversation.user_id)
                     << " updated_at=" << SanitizeForLog(FormatTimestamp(updated_at)) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return status;
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistConversationMessage(std::string_view turn_id,
                                                            const Message& message) {
    if (!store_) {
        return absl::OkStatus();
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return invalid_argument(
            "cannot persist a conversation message without a conversation item");
    }
    const ConversationItem& current_item = conversation.items.back();
    if (current_item.type != ConversationItemType::OngoingEpisode ||
        !current_item.ongoing_episode.has_value() ||
        current_item.ongoing_episode->messages.empty()) {
        return invalid_argument("cannot persist a conversation message without an ongoing episode");
    }

    const std::int64_t conversation_item_index =
        static_cast<std::int64_t>(conversation.items.size() - 1U);
    const std::int64_t message_index =
        static_cast<std::int64_t>(current_item.ongoing_episode->messages.size() - 1U);
    const ConversationMessageWrite write{
        .session_id = session_id_,
        .conversation_item_index = conversation_item_index,
        .message_index = message_index,
        .turn_id = std::string(turn_id),
        .role = message.role,
        .content = message.content,
        .create_time = message.create_time,
    };
    if (absl::Status status = ValidateConversationMessageWrite(write); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->AppendConversationMessage(write); !status.ok()) {
        LOG(WARNING)
            << "MemoryOrchestrator store.AppendConversationMessage failed while appending the raw "
               "transcript message for the current ongoing episode"
            << " session_id=" << SanitizeForLog(session_id_)
            << " turn_id=" << SanitizeForLog(turn_id)
            << " role=" << (message.role == MessageRole::User ? "user" : "assistant")
            << " conversation_item_index=" << conversation_item_index
            << " message_index=" << message_index
            << " message_created_at=" << SanitizeForLog(FormatTimestamp(message.create_time))
            << " detail='" << SanitizeForLog(status.message()) << "'";
        return status;
    }
    return absl::OkStatus();
}

absl::Status
MemoryOrchestrator::PersistCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush) {
    if (!store_) {
        return absl::OkStatus();
    }
    if (absl::Status session_status = ValidateSessionReadyForPersistence(); !session_status.ok()) {
        return session_status;
    }

    const MidTermEpisodeWrite episode_write{
        .session_id = session_id_,
        .source_conversation_item_index = static_cast<std::int64_t>(flush.conversation_item_index),
        .episode = flush.episode,
    };
    if (absl::Status status = ValidateMidTermEpisodeWrite(episode_write); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->UpsertMidTermEpisode(episode_write); !status.ok()) {
        LOG(WARNING)
            << "MemoryOrchestrator store.UpsertMidTermEpisode failed while persisting the flushed "
               "mid-term episode before conversation item replacement"
            << " session_id=" << SanitizeForLog(session_id_)
            << " episode_id=" << SanitizeForLog(flush.episode.episode_id)
            << " source_conversation_item_index=" << flush.conversation_item_index
            << " episode_created_at=" << SanitizeForLog(FormatTimestamp(flush.episode.created_at))
            << " detail='" << SanitizeForLog(status.message()) << "'";
        return status;
    }

    if (flush.split_at_message_index.has_value()) {
        // For split flushes, read the remaining messages from the live conversation so the
        // persistence layer can reconstruct the full state on reload.
        const Conversation& conversation = memory_.conversation();
        const auto& item = conversation.items[flush.conversation_item_index];
        if (!item.ongoing_episode.has_value()) {
            return absl::InternalError(
                "PersistCompletedEpisodeFlush expected an ongoing episode for split persistence");
        }
        const auto& messages = item.ongoing_episode->messages;
        OngoingEpisode remaining;
        remaining.messages.assign(messages.begin() +
                                      static_cast<std::ptrdiff_t>(*flush.split_at_message_index),
                                  messages.end());

        const SplitEpisodeStubWrite split_write{
            .session_id = session_id_,
            .conversation_item_index = static_cast<std::int64_t>(flush.conversation_item_index),
            .episode_id = flush.episode.episode_id,
            .episode_stub_content = flush.episode.tier3_ref,
            .episode_stub_create_time = flush.stub_timestamp,
            .remaining_ongoing_episode = std::move(remaining),
        };
        if (absl::Status status = ValidateSplitEpisodeStubWrite(split_write); !status.ok()) {
            return status;
        }
        if (absl::Status status = store_->SplitConversationItemWithEpisodeStub(split_write);
            !status.ok()) {
            LOG(WARNING)
                << "MemoryOrchestrator store.SplitConversationItemWithEpisodeStub failed while "
                   "persisting a split episode stub"
                << " session_id=" << SanitizeForLog(session_id_)
                << " episode_id=" << SanitizeForLog(flush.episode.episode_id)
                << " conversation_item_index=" << flush.conversation_item_index
                << " split_at_message_index=" << *flush.split_at_message_index
                << " remaining_message_count="
                << split_write.remaining_ongoing_episode.messages.size() << " detail='"
                << SanitizeForLog(status.message()) << "'";
            return status;
        }
    } else {
        const EpisodeStubWrite stub_write{
            .session_id = session_id_,
            .conversation_item_index = static_cast<std::int64_t>(flush.conversation_item_index),
            .episode_id = flush.episode.episode_id,
            .episode_stub_content = flush.episode.tier3_ref,
            .episode_stub_create_time = flush.stub_timestamp,
        };
        if (absl::Status status = ValidateEpisodeStubWrite(stub_write); !status.ok()) {
            return status;
        }
        if (absl::Status status = store_->ReplaceConversationItemWithEpisodeStub(stub_write);
            !status.ok()) {
            LOG(WARNING)
                << "MemoryOrchestrator store.ReplaceConversationItemWithEpisodeStub failed while "
                   "marking the flushed conversation item as an episode stub"
                << " session_id=" << SanitizeForLog(session_id_)
                << " episode_id=" << SanitizeForLog(flush.episode.episode_id)
                << " conversation_item_index=" << flush.conversation_item_index
                << " episode_stub_created_at="
                << SanitizeForLog(FormatTimestamp(flush.stub_timestamp)) << " detail='"
                << SanitizeForLog(status.message()) << "'";
            return status;
        }
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<RetrievedMemory>>
MemoryOrchestrator::RetrieveRelevantMemories(const Message& /*user_message*/) {
    if (!store_) {
        return std::nullopt;
    }

    const PersistentMemoryCache& cache = memory_.snapshot().system_prompt.persistent_memory_cache;
    if (cache.active_models.empty() && cache.familiar_labels.empty()) {
        return std::nullopt;
    }

    // Collect entity ids from the persistent cache to query relationships and episodes.
    std::vector<std::string> entity_ids;
    entity_ids.reserve(cache.active_models.size() + cache.familiar_labels.size());
    for (const ActiveModel& model : cache.active_models) {
        entity_ids.push_back(model.entity_id);
    }
    for (const FamiliarLabel& label : cache.familiar_labels) {
        entity_ids.push_back(label.entity_id);
    }

    std::string context;

    for (const std::string& entity_id : entity_ids) {
        const absl::StatusOr<std::vector<Relationship>> relationships =
            store_->ListRelationshipsForEntity(entity_id);
        if (!relationships.ok()) {
            if (!absl::IsUnimplemented(relationships.status())) {
                LOG(WARNING) << "MemoryOrchestrator failed to retrieve relationships for entity"
                             << " session_id=" << SanitizeForLog(session_id_)
                             << " entity_id=" << SanitizeForLog(entity_id) << " detail='"
                             << SanitizeForLog(relationships.status().message()) << "'";
            }
            continue;
        }

        for (const Relationship& rel : *relationships) {
            context.append("- ");
            context.append(rel.from_entity_id);
            context.append(" ");
            context.append(rel.predicate);
            context.append(" ");
            context.append(rel.to_entity_id);
            context.push_back('\n');
        }

        const absl::StatusOr<std::vector<LongTermEpisode>> episodes =
            store_->ListLongTermEpisodesForEntity(entity_id);
        if (!episodes.ok()) {
            if (!absl::IsUnimplemented(episodes.status())) {
                LOG(WARNING)
                    << "MemoryOrchestrator failed to retrieve long-term episodes for entity"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " entity_id=" << SanitizeForLog(entity_id) << " detail='"
                    << SanitizeForLog(episodes.status().message()) << "'";
            }
            continue;
        }

        for (const LongTermEpisode& episode : *episodes) {
            context.append("- [episode] ");
            context.append(episode.summary_compressed);
            context.push_back('\n');
        }
    }

    if (context.empty()) {
        return std::nullopt;
    }
    return context;
}

bool MemoryOrchestrator::ShouldQueueMidTermAnalysisAfterAssistantReply() const {
    return mid_term_flush_decider_ != nullptr && user_turns_since_last_mid_term_decider_run_ >=
                                                     mid_term_flush_decider_interval_user_turns_;
}

void MemoryOrchestrator::NoteUserTurnAppended() {
    if (mid_term_flush_decider_ == nullptr) {
        return;
    }
    ++user_turns_since_last_mid_term_decider_run_;
}

void MemoryOrchestrator::NoteMidTermAnalysisQueued() {
    user_turns_since_last_mid_term_decider_run_ = 0U;
}

std::string MemoryOrchestrator::NextEpisodeId() {
    return "ep_" + session_id_ + "_" + std::to_string(next_episode_sequence_++);
}

absl::StatusOr<SleepCycleExtractionResult>
MemoryOrchestrator::BuildSleepCycleExtractionResult(std::string_view user_id,
                                                    const std::vector<Episode>& mid_term_episodes) {
    if (user_id.empty()) {
        return invalid_argument("sleep-cycle extraction requires a non-empty user_id");
    }

    SleepCycleExtractionResult result;
    result.long_term_episodes.reserve(mid_term_episodes.size());

    for (const Episode& episode : mid_term_episodes) {
        result.long_term_episodes.push_back(LongTermEpisodeWrite{
            .episode =
                LongTermEpisode{
                    .lte_id = "lte_" + episode.episode_id,
                    .user_id = std::string(user_id),
                    .summary_full = episode.tier1_detail,
                    .summary_compressed = episode.tier2_summary,
                    .keywords = episode.tier3_keywords,
                    .embedding = episode.embedding.empty()
                                     ? std::nullopt
                                     : std::optional<Embedding>(episode.embedding),
                    .complexity = episode.salience,
                    .created_at = episode.created_at,
                    .original_episode_ids = { episode.episode_id },
                },
        });
    }

    if (absl::Status status = ValidateSleepCycleExtractionResult(result); !status.ok()) {
        return status;
    }
    return result;
}

absl::StatusOr<std::size_t>
MemoryOrchestrator::ConsolidateToLongTerm(const std::vector<Episode>& mid_term_episodes) {
    if (store_ == nullptr || mid_term_episodes.empty()) {
        return 0;
    }

    const std::string& user_id = memory_.snapshot().conversation.user_id;
    absl::StatusOr<SleepCycleExtractionResult> extraction =
        BuildSleepCycleExtractionResult(user_id, mid_term_episodes);
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
              << " mid-term episodes to "
                 "long-term storage"
              << " session_id=" << SanitizeForLog(session_id_);

    return extraction->long_term_episodes.size();
}

absl::StatusOr<MemoryOrchestrator::CompletedFlushBuildInput>
MemoryOrchestrator::CompactFlushCandidate(const MidTermCompactorPtr& compactor,
                                          std::string_view session_id,
                                          const OngoingEpisodeFlushCandidate& flush_candidate,
                                          std::string_view failure_context) {
    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MidTermCompactionRequest{
            .session_id = std::string(session_id),
            .flush_candidate = flush_candidate,
        });
    if (!compacted.ok()) {
        return compacted.status();
    }
    if (compacted->tier2_summary.empty() || compacted->tier3_ref.empty()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid " << failure_context
                     << " compactor "
                        "output"
                     << " session_id=" << SanitizeForLog(session_id)
                     << " detail='mid-term compactor must produce non-empty tier2 and tier3 "
                        "content'";
        return absl::InvalidArgumentError(
            "mid-term compactor must produce non-empty tier2 and tier3 content");
    }

    const Timestamp episode_time = flush_candidate.ongoing_episode.messages.back().create_time;
    return CompletedFlushBuildInput{
        .compacted = std::move(*compacted),
        .episode_created_at = episode_time,
        .stub_timestamp = episode_time,
        .segment_message_count = flush_candidate.ongoing_episode.messages.size(),
    };
}

CompletedOngoingEpisodeFlush
MemoryOrchestrator::BuildCompletedEpisodeFlush(std::size_t conversation_item_index,
                                               CompletedFlushBuildInput build_input,
                                               std::optional<std::size_t> split_at_message_index) {
    return CompletedOngoingEpisodeFlush{
        .conversation_item_index = conversation_item_index,
        .episode =
            Episode{
                .episode_id = NextEpisodeId(),
                .tier1_detail = std::move(build_input.compacted.tier1_detail),
                .tier2_summary = std::move(build_input.compacted.tier2_summary),
                .tier3_ref = std::move(build_input.compacted.tier3_ref),
                .tier3_keywords = std::move(build_input.compacted.tier3_keywords),
                .salience = build_input.compacted.salience,
                .embedding = std::move(build_input.compacted.embedding),
                .created_at = build_input.episode_created_at,
            },
        .stub_timestamp = build_input.stub_timestamp,
        .split_at_message_index = split_at_message_index,
    };
}

absl::Status MemoryOrchestrator::QueueMidTermAnalysis(const Conversation& conversation_snapshot) {
    if (mid_term_compactor_ == nullptr || mid_term_flush_decider_ == nullptr) {
        return absl::OkStatus();
    }

    MidTermFlushDeciderPtr decider = mid_term_flush_decider_;
    MidTermCompactorPtr compactor = mid_term_compactor_;
    const std::string session_id = session_id_;
    const std::optional<std::size_t> conversation_item_index =
        conversation_snapshot.items.empty()
            ? std::nullopt
            : std::optional<std::size_t>(conversation_snapshot.items.size() - 1U);
    pending_mid_term_flushes_.push_back(PendingMidTermFlush{
        .conversation_item_index = conversation_item_index,
        .future = std::async(
            std::launch::async,
            [decider = std::move(decider), compactor = std::move(compactor), session_id,
             conversation_snapshot]() -> absl::StatusOr<AsyncMidTermFlushResult> {
                const absl::StatusOr<MidTermFlushDecision> decision =
                    decider->Decide(conversation_snapshot);
                if (!decision.ok()) {
                    return decision.status();
                }

                const absl::StatusOr<std::optional<std::size_t>> target_item_index =
                    FindLiveConversationItemIndex(conversation_snapshot, *decision, session_id);
                if (!target_item_index.ok()) {
                    return target_item_index.status();
                }
                if (!target_item_index->has_value()) {
                    return AsyncMidTermFlushResult{};
                }

                absl::StatusOr<std::vector<OngoingEpisodeFlushCandidate>> candidates =
                    CaptureOngoingEpisodeSegmentsForFlush(conversation_snapshot,
                                                          **target_item_index, *decision);
                if (!candidates.ok()) {
                    return candidates.status();
                }

                std::vector<CompletedFlushBuildInput> completed_flushes;
                completed_flushes.reserve(candidates->size());
                for (const OngoingEpisodeFlushCandidate& candidate : *candidates) {
                    absl::StatusOr<CompletedFlushBuildInput> build_input =
                        MemoryOrchestrator::CompactFlushCandidate(compactor, session_id, candidate,
                                                                  "mid-term");
                    if (!build_input.ok()) {
                        return build_input.status();
                    }
                    completed_flushes.push_back(std::move(*build_input));
                }
                return AsyncMidTermFlushResult{
                    .completed_flushes = std::move(completed_flushes),
                    .captured_message_count = conversation_snapshot.items[**target_item_index]
                                                  .ongoing_episode->messages.size(),
                    .tail_complete = decision->tail_complete,
                };
            }),
        .freeze_tail_before_append = false,
    });
    VLOG(1) << "MemoryOrchestrator queued async mid-term analysis session_id="
            << SanitizeForLog(session_id_)
            << " snapshot_item_count=" << conversation_snapshot.items.size();
    return absl::OkStatus();
}

absl::Status
MemoryOrchestrator::QueueMidTermFlush(const OngoingEpisodeFlushCandidate& flush_candidate,
                                      std::optional<std::size_t> split_at_message_index) {
    if (mid_term_compactor_ == nullptr) {
        return absl::OkStatus();
    }

    const std::string session_id = session_id_;
    MidTermCompactorPtr compactor = mid_term_compactor_;
    pending_mid_term_flushes_.push_back(PendingMidTermFlush{
        .conversation_item_index = flush_candidate.conversation_item_index,
        .future = std::async(std::launch::async,
                             [compactor = std::move(compactor), session_id, split_at_message_index,
                              flush_candidate = OngoingEpisodeFlushCandidate(
                                  flush_candidate)]() -> absl::StatusOr<AsyncMidTermFlushResult> {
                                 absl::StatusOr<CompletedFlushBuildInput> build_input =
                                     MemoryOrchestrator::CompactFlushCandidate(
                                         compactor, session_id, flush_candidate, "mid-term");
                                 if (!build_input.ok()) {
                                     return build_input.status();
                                 }
                                 return AsyncMidTermFlushResult{
                                     .completed_flushes = { std::move(*build_input) },
                                     .captured_message_count =
                                         flush_candidate.ongoing_episode.messages.size(),
                                     .tail_complete = !split_at_message_index.has_value(),
                                 };
                             }),
        .freeze_tail_before_append = !split_at_message_index.has_value(),
    });
    VLOG(1) << "MemoryOrchestrator queued async mid-term flush session_id="
            << SanitizeForLog(session_id_)
            << " conversation_item_index=" << flush_candidate.conversation_item_index
            << " was_split=" << (split_at_message_index.has_value() ? "true" : "false");
    return absl::OkStatus();
}

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

absl::StatusOr<std::size_t> MemoryOrchestrator::DrainCompletedMidTermCompactions() {
    std::size_t drained_count = 0;
    for (auto it = pending_mid_term_flushes_.begin(); it != pending_mid_term_flushes_.end();) {
        if (it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        absl::StatusOr<AsyncMidTermFlushResult> result = it->future.get();
        const std::optional<std::size_t> adjusted_conversation_item_index =
            it->conversation_item_index;
        it = pending_mid_term_flushes_.erase(it);
        if (!result.ok()) {
            LOG(WARNING) << "MemoryOrchestrator async mid-term flush failed session_id="
                         << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(result.status().message()) << "'";
            return result.status();
        }
        if (result->completed_flushes.empty()) {
            VLOG(1) << "MemoryOrchestrator drained completed async mid-term analysis with no flush"
                    << " session_id=" << SanitizeForLog(session_id_);
            continue;
        }
        if (!adjusted_conversation_item_index.has_value()) {
            return invalid_argument(
                "completed async mid-term flush is missing its target conversation item index");
        }

        bool tail_complete = result->tail_complete;
        if (tail_complete &&
            *adjusted_conversation_item_index < memory_.conversation().items.size()) {
            const ConversationItem& live_item =
                memory_.conversation().items[*adjusted_conversation_item_index];
            if (live_item.type == ConversationItemType::OngoingEpisode &&
                live_item.ongoing_episode.has_value() &&
                live_item.ongoing_episode->messages.size() > result->captured_message_count) {
                tail_complete = false;
                VLOG(1) << "MemoryOrchestrator downgraded completed async tail flush to leave a "
                           "live tail because new messages were appended while compaction was "
                           "running session_id="
                        << SanitizeForLog(session_id_)
                        << " conversation_item_index=" << *adjusted_conversation_item_index
                        << " captured_message_count=" << result->captured_message_count
                        << " live_message_count=" << live_item.ongoing_episode->messages.size();
            }
        }

        std::size_t current_conversation_item_index = *adjusted_conversation_item_index;
        for (std::size_t flush_index = 0; flush_index < result->completed_flushes.size();
             ++flush_index) {
            const bool is_last_flush = flush_index + 1U == result->completed_flushes.size();
            std::optional<std::size_t> split_at_message_index =
                (!is_last_flush || !tail_complete)
                    ? std::optional<std::size_t>(
                          result->completed_flushes[flush_index].segment_message_count)
                    : std::nullopt;

            CompletedOngoingEpisodeFlush completed_flush = BuildCompletedEpisodeFlush(
                current_conversation_item_index, std::move(result->completed_flushes[flush_index]),
                split_at_message_index);

            if (absl::Status status = ValidateMidTermEpisodeWrite(MidTermEpisodeWrite{
                    .session_id = session_id_,
                    .source_conversation_item_index =
                        static_cast<std::int64_t>(completed_flush.conversation_item_index),
                    .episode = completed_flush.episode,
                });
                !status.ok()) {
                return status;
            }

            const bool was_split = completed_flush.split_at_message_index.has_value();
            if (absl::Status apply_status = ApplyCompletedEpisodeFlush(completed_flush);
                !apply_status.ok()) {
                return apply_status;
            }
            VLOG(1) << "MemoryOrchestrator drained completed async mid-term flush session_id="
                    << SanitizeForLog(session_id_)
                    << " episode_id=" << SanitizeForLog(completed_flush.episode.episode_id)
                    << " conversation_item_index=" << completed_flush.conversation_item_index
                    << " was_split=" << (was_split ? "true" : "false");
            if (was_split) {
                ++current_conversation_item_index;
            }
        }
        ++drained_count;
    }
    return drained_count;
}

absl::StatusOr<std::size_t> MemoryOrchestrator::AwaitAndDrainAllPendingMidTermCompactions() {
    static constexpr auto kMaxWait = std::chrono::seconds(30);

    if (pending_mid_term_flushes_.empty()) {
        return 0U;
    }
    VLOG(1) << "MemoryOrchestrator awaiting all pending mid-term compactions session_id="
            << SanitizeForLog(session_id_) << " pending_count=" << pending_mid_term_flushes_.size();
    for (auto& pending : pending_mid_term_flushes_) {
        if (pending.future.wait_for(kMaxWait) != std::future_status::ready) {
            LOG(ERROR) << "MemoryOrchestrator timed out awaiting pending mid-term compaction"
                       << " session_id=" << SanitizeForLog(session_id_)
                       << " deadline_seconds=" << kMaxWait.count();
            return absl::DeadlineExceededError(
                "timed out awaiting pending mid-term compaction to complete");
        }
    }
    return DrainCompletedMidTermCompactions();
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

    // Consolidate mid-term episodes to long-term storage before clearing. If any upsert fails,
    // abort the sleep cycle to avoid dropping unconsolidated episodes.
    // TODO: The caller that invokes RunSleepCycle should implement retry with backoff so that
    // transient store failures do not leave a user's session permanently clogged.
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

    // Persist the post-sleep snapshot first so failures do not strand the live session after the
    // tables and in-memory working set have already been cleared.
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

bool MemoryOrchestrator::HasPendingMidTermCompactions() const {
    return !pending_mid_term_flushes_.empty();
}

void MemoryOrchestrator::PrepareConversationForAppend() {
    if (pending_mid_term_flushes_.empty()) {
        return;
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return;
    }

    const std::size_t last_conversation_item_index = conversation.items.size() - 1U;
    for (const PendingMidTermFlush& pending_flush : pending_mid_term_flushes_) {
        if (pending_flush.freeze_tail_before_append &&
            pending_flush.conversation_item_index.has_value() &&
            *pending_flush.conversation_item_index == last_conversation_item_index) {
            VLOG(1) << "MemoryOrchestrator started a new ongoing episode before append because the "
                       "tail conversation item is still flushing"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " pending_conversation_item_index="
                    << *pending_flush.conversation_item_index;
            BeginOngoingEpisode(memory_.mutable_conversation());
            return;
        }
    }
}

absl::Status MemoryOrchestrator::AfterUserQueryAppended(const Message& user_message) {
    NoteUserTurnAppended();
    absl::StatusOr<std::optional<RetrievedMemory>> retrieved_memory =
        RetrieveRelevantMemories(user_message);
    if (!retrieved_memory.ok()) {
        return retrieved_memory.status();
    }
    memory_.SetRetrievedMemory(std::move(*retrieved_memory));
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::AfterAssistantReplyAppended(const Message& assistant_message) {
    if (mid_term_compactor_ == nullptr) {
        return absl::OkStatus();
    }
    if (assistant_message.role != MessageRole::Assistant) {
        return invalid_argument("flush capture currently requires an assistant message");
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return absl::OkStatus();
    }

    if (mid_term_flush_decider_ != nullptr) {
        if (!ShouldQueueMidTermAnalysisAfterAssistantReply()) {
            VLOG(1) << "MemoryOrchestrator deferred async mid-term analysis until more user turns "
                       "accumulate"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " user_turns_since_last_decider_run="
                    << user_turns_since_last_mid_term_decider_run_
                    << " required_user_turns=" << mid_term_flush_decider_interval_user_turns_;
            return absl::OkStatus();
        }
        if (!pending_mid_term_flushes_.empty()) {
            VLOG(1) << "MemoryOrchestrator skipped async mid-term analysis queue because another "
                       "analysis or flush is already pending"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " pending_count=" << pending_mid_term_flushes_.size();
            return absl::OkStatus();
        }
        if (absl::Status status = QueueMidTermAnalysis(conversation); !status.ok()) {
            return status;
        }
        NoteMidTermAnalysisQueued();
        return absl::OkStatus();
    }

    const std::size_t conversation_item_index = conversation.items.size() - 1U;
    for (const PendingMidTermFlush& pending_flush : pending_mid_term_flushes_) {
        if (pending_flush.conversation_item_index.has_value() &&
            *pending_flush.conversation_item_index == conversation_item_index) {
            VLOG(1) << "MemoryOrchestrator skipped duplicate async mid-term flush queue for the "
                       "same conversation item"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " conversation_item_index=" << conversation_item_index;
            return absl::OkStatus();
        }
    }

    const ConversationItem& item = conversation.items.back();
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return absl::OkStatus();
    }
    if (item.ongoing_episode->messages.size() < 2U) {
        return absl::OkStatus();
    }

    absl::StatusOr<OngoingEpisodeFlushCandidate> candidate =
        memory_.CaptureOngoingEpisodeForFlush(conversation_item_index);
    if (!candidate.ok()) {
        return candidate.status();
    }
    if (absl::Status queue_status = QueueMidTermFlush(*candidate); !queue_status.ok()) {
        return queue_status;
    }
    // TODO: Apply assistant-side memory updates here (write-back caching, salience updates, etc.).
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::HandleConversationMessage(std::string_view session_id,
                                                           std::string_view turn_id,
                                                           std::string_view text,
                                                           Timestamp create_time,
                                                           MessageRole role) {
    if (absl::Status validation_status =
            ValidateTurnText(session_id, turn_id, role == MessageRole::User ? "user" : "assistant");
        !validation_status.ok()) {
        return validation_status;
    }
    if (absl::Status session_status = ValidateSessionReadyForPersistence(); !session_status.ok()) {
        return session_status;
    }
    const absl::StatusOr<std::size_t> drained_flushes = DrainCompletedMidTermCompactions();
    if (!drained_flushes.ok()) {
        return drained_flushes.status();
    }
    PrepareConversationForAppend();

    if (role == MessageRole::User) {
        AppendUserMessage(memory_.mutable_conversation(), std::string(text), create_time);
        const Message& user_message =
            memory_.conversation().items.back().ongoing_episode->messages.back();
        if (absl::Status persistence_status = PersistConversationMessage(turn_id, user_message);
            !persistence_status.ok()) {
            return persistence_status;
        }
        if (absl::Status post_status = AfterUserQueryAppended(user_message); !post_status.ok()) {
            return post_status;
        }
    } else {
        AppendAssistantMessage(memory_.mutable_conversation(), std::string(text), create_time);
        const Message& assistant_message =
            memory_.conversation().items.back().ongoing_episode->messages.back();
        if (absl::Status persistence_status =
                PersistConversationMessage(turn_id, assistant_message);
            !persistence_status.ok()) {
            return persistence_status;
        }
        if (absl::Status post_status = AfterAssistantReplyAppended(assistant_message);
            !post_status.ok()) {
            return post_status;
        }
    }
    if (absl::Status snapshot_status = PersistUserWorkingMemorySnapshot(create_time);
        !snapshot_status.ok()) {
        return snapshot_status;
    }

    VLOG(1) << "MemoryOrchestrator handled conversation message session_id="
            << SanitizeForLog(session_id_) << " turn_id=" << SanitizeForLog(turn_id)
            << " role=" << (role == MessageRole::User ? "user" : "assistant");
    return absl::OkStatus();
}

absl::StatusOr<UserQueryMemoryResult>
MemoryOrchestrator::HandleUserQuery(const GatewayUserQuery& query) {
    if (absl::Status status = HandleConversationMessage(query.session_id, query.turn_id, query.text,
                                                        query.create_time, MessageRole::User);
        !status.ok()) {
        return status;
    }

    absl::StatusOr<RenderedWorkingMemory> rendered_bundle = memory_.RenderPromptBundle();
    if (!rendered_bundle.ok()) {
        return rendered_bundle.status();
    }

    return UserQueryMemoryResult{
        .rendered_system_prompt = std::move(rendered_bundle->system_prompt),
        .rendered_working_memory_context = std::move(rendered_bundle->context),
        .rendered_working_memory = std::move(rendered_bundle->full_prompt),
    };
}

absl::Status MemoryOrchestrator::HandleAssistantReply(const GatewayAssistantReply& reply) {
    return HandleConversationMessage(reply.session_id, reply.turn_id, reply.text, reply.create_time,
                                     MessageRole::Assistant);
}

absl::Status
MemoryOrchestrator::ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush) {
    // Validate that the target conversation item is still a valid ongoing episode without copying
    // any message data — the full capture is unnecessary at apply time.
    if (flush.split_at_message_index.has_value()) {
        if (const absl::Status status = memory_.ValidateOngoingEpisodeForSplitFlush(
                flush.conversation_item_index, *flush.split_at_message_index);
            !status.ok()) {
            return status;
        }
    } else {
        if (const absl::Status status =
                memory_.ValidateOngoingEpisodeForFlush(flush.conversation_item_index);
            !status.ok()) {
            return status;
        }
    }

    if (absl::Status persistence_status = PersistCompletedEpisodeFlush(flush);
        !persistence_status.ok()) {
        return persistence_status;
    }
    if (absl::Status apply_status = memory_.ApplyCompletedOngoingEpisodeFlush(flush);
        !apply_status.ok()) {
        return apply_status;
    }

    // A split insert adds a new conversation item after the stub. Adjust indices of any remaining
    // pending flushes that reference conversation items after the split point.
    if (flush.split_at_message_index.has_value()) {
        for (auto& pending : pending_mid_term_flushes_) {
            if (pending.conversation_item_index.has_value() &&
                *pending.conversation_item_index > flush.conversation_item_index) {
                *pending.conversation_item_index += 1;
            }
        }
    }
    return PersistUserWorkingMemorySnapshot(flush.stub_timestamp);
}

absl::StatusOr<std::string> MemoryOrchestrator::RenderFullWorkingMemory() const {
    return memory_.RenderFullWorkingMemory();
}

absl::StatusOr<std::string> MemoryOrchestrator::RenderSystemPrompt() const {
    return memory_.RenderSystemPrompt();
}

absl::StatusOr<std::string> MemoryOrchestrator::RenderWorkingMemoryContext() const {
    return memory_.RenderWorkingMemoryContext();
}

absl::StatusOr<std::string>
MemoryOrchestrator::ExpandMidTermEpisode(std::string_view episode_id) const {
    for (const Episode& episode : memory_.snapshot().mid_term_episodes) {
        if (episode.episode_id != episode_id) {
            continue;
        }
        if (!IsExpandableEpisode(episode)) {
            return absl::FailedPreconditionError(
                "mid-term episode does not have expandable Tier 1 detail available");
        }
        return *episode.tier1_detail;
    }
    return absl::NotFoundError("mid-term episode was not found");
}

} // namespace isla::server::memory
