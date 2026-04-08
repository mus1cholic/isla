#include "isla/server/memory/memory_orchestrator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

absl::StatusOr<MemoryOrchestrator::CompletedFlushBuildInput>
MemoryOrchestrator::CompactFlushCandidate(
    const MidTermCompactorPtr& compactor, std::string_view session_id,
    const OngoingEpisodeFlushCandidate& flush_candidate, std::string_view failure_context,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MidTermCompactionRequest{
            .session_id = std::string(session_id),
            .flush_candidate = flush_candidate,
            .telemetry_context = std::move(telemetry_context),
        });
    if (!compacted.ok()) {
        return compacted.status();
    }
    if (compacted->tier2_summary.empty() || compacted->tier3_ref.empty()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid " << failure_context
                     << " compactor output"
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

absl::Status MemoryOrchestrator::QueueMidTermAnalysis(
    const Conversation& conversation_snapshot,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
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
             conversation_snapshot, telemetry_context = std::move(telemetry_context)]()
                -> absl::StatusOr<AsyncMidTermFlushResult> {
                const absl::StatusOr<MidTermFlushDecision> decision =
                    decider->Decide(conversation_snapshot, telemetry_context);
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
                                                                  "mid-term", telemetry_context);
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

absl::Status MemoryOrchestrator::QueueMidTermFlush(
    const OngoingEpisodeFlushCandidate& flush_candidate,
    std::optional<std::size_t> split_at_message_index,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
    if (mid_term_compactor_ == nullptr) {
        return absl::OkStatus();
    }

    const std::string session_id = session_id_;
    MidTermCompactorPtr compactor = mid_term_compactor_;
    pending_mid_term_flushes_.push_back(PendingMidTermFlush{
        .conversation_item_index = flush_candidate.conversation_item_index,
        .future = std::async(
            std::launch::async,
            [compactor = std::move(compactor), session_id, split_at_message_index,
             flush_candidate = OngoingEpisodeFlushCandidate(flush_candidate),
             telemetry_context =
                 std::move(telemetry_context)]() -> absl::StatusOr<AsyncMidTermFlushResult> {
                absl::StatusOr<CompletedFlushBuildInput> build_input =
                    MemoryOrchestrator::CompactFlushCandidate(
                        compactor, session_id, flush_candidate, "mid-term", telemetry_context);
                if (!build_input.ok()) {
                    return build_input.status();
                }
                return AsyncMidTermFlushResult{
                    .completed_flushes = { std::move(*build_input) },
                    .captured_message_count = flush_candidate.ongoing_episode.messages.size(),
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

absl::Status
MemoryOrchestrator::ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush) {
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

} // namespace isla::server::memory
