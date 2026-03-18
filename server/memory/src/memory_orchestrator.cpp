#include "isla/server/memory/memory_orchestrator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/memory/conversation.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

struct FlushTarget {
    std::size_t conversation_item_index = 0;
    std::optional<std::size_t> split_at_message_index;
};

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
    const auto& messages = item.ongoing_episode->messages;
    if (split_at_message_index >= messages.size()) {
        return invalid_argument("split_at_message_index exceeds message count");
    }
    if (split_at_message_index < 2) {
        return invalid_argument(
            "split_at_message_index must leave at least 2 messages in the completed portion");
    }
    if (messages[split_at_message_index].role != MessageRole::User) {
        return invalid_argument("split_at_message_index must reference a user message");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<FlushTarget>>
ChooseFlushConversationItem(const Conversation& conversation, const MidTermFlushDecision& decision,
                            std::string_view session_id) {
    if (conversation.items.empty()) {
        return std::nullopt;
    }

    if (!decision.should_flush) {
        if (decision.conversation_item_index.has_value()) {
            LOG(WARNING)
                << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                << " session_id=" << SanitizeForLog(session_id)
                << " detail='flush decider returned a conversation item while should_flush was "
                   "false'";
            return invalid_argument("mid-term flush decider cannot return a conversation item when "
                                    "should_flush is false");
        }
        VLOG(1) << "MemoryOrchestrator flush decider chose not to flush session_id="
                << SanitizeForLog(session_id);
        return std::nullopt;
    }
    if (!decision.conversation_item_index.has_value()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                     << " session_id=" << SanitizeForLog(session_id)
                     << " detail='flush decider requested a flush without a conversation item'";
        return invalid_argument(
            "mid-term flush decider must return a conversation item when should_flush is true");
    }
    const std::size_t item_index = *decision.conversation_item_index;
    if (item_index >= conversation.items.size()) {
        LOG(WARNING)
            << "MemoryOrchestrator rejected invalid mid-term flush decider output"
            << " session_id=" << SanitizeForLog(session_id)
            << " conversation_item_index=" << item_index
            << " conversation_size=" << conversation.items.size()
            << " detail='flush decider returned a conversation item outside the conversation "
               "range'";
        return invalid_argument(
            "mid-term flush decider returned a conversation item outside the conversation range");
    }

    if (decision.split_at_message_index.has_value()) {
        const std::size_t split_at = *decision.split_at_message_index;
        if (absl::Status status = ValidateSplitFlushTarget(conversation, item_index, split_at);
            !status.ok()) {
            LOG(WARNING) << "MemoryOrchestrator rejected invalid split flush target from decider"
                         << " session_id=" << SanitizeForLog(session_id)
                         << " conversation_item_index=" << item_index
                         << " split_at_message_index=" << split_at << " detail='"
                         << SanitizeForLog(status.message()) << "'";
            return status;
        }
    }

    VLOG(1) << "MemoryOrchestrator flush decider chose a conversation item for flush session_id="
            << SanitizeForLog(session_id) << " conversation_item_index=" << item_index
            << " split_at="
            << (decision.split_at_message_index.has_value()
                    ? std::to_string(*decision.split_at_message_index)
                    : "none");
    return FlushTarget{
        .conversation_item_index = item_index,
        .split_at_message_index = decision.split_at_message_index,
    };
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

absl::StatusOr<OngoingEpisodeFlushCandidate>
CaptureOngoingEpisodeForSplitFlush(const Conversation& conversation,
                                   std::size_t conversation_item_index,
                                   std::size_t split_at_message_index) {
    if (absl::Status status =
            ValidateSplitFlushTarget(conversation, conversation_item_index, split_at_message_index);
        !status.ok()) {
        return status;
    }

    const auto& messages = conversation.items[conversation_item_index].ongoing_episode->messages;
    OngoingEpisode completed_portion;
    completed_portion.messages.assign(
        messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(split_at_message_index));
    return OngoingEpisodeFlushCandidate{
        .conversation_item_index = conversation_item_index,
        .ongoing_episode = std::move(completed_portion),
    };
}

} // namespace

MemoryOrchestrator::MemoryOrchestrator(std::string session_id, WorkingMemory memory,
                                       MemoryStorePtr store,
                                       MidTermFlushDeciderPtr mid_term_flush_decider,
                                       MidTermCompactorPtr mid_term_compactor)
    : session_id_(std::move(session_id)), memory_(std::move(memory)), store_(std::move(store)),
      mid_term_flush_decider_(std::move(mid_term_flush_decider)),
      mid_term_compactor_(std::move(mid_term_compactor)), pending_mid_term_flushes_(),
      next_episode_sequence_(1), session_persisted_(false) {}

absl::StatusOr<MemoryOrchestrator> MemoryOrchestrator::Create(std::string session_id,
                                                              const MemoryOrchestratorInit& init) {
    if (session_id.empty()) {
        return invalid_argument("memory orchestrator must include a session_id");
    }

    absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
        .system_prompt = "",
        .user_id = init.user_id,
    });
    if (!memory.ok()) {
        return memory.status();
    }
    return MemoryOrchestrator(std::move(session_id), std::move(*memory), init.store,
                              init.mid_term_flush_decider, init.mid_term_compactor);
}

absl::Status MemoryOrchestrator::BeginSession(Timestamp create_time) {
    return PersistSessionIfNeeded(create_time);
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
    session_persisted_ = true;
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
MemoryOrchestrator::RetrieveRelevantMemories(const Message& user_message) {
    static_cast<void>(user_message);
    // TODO: Query mid-term and long-term memory systems here before prompt rendering.
    return std::nullopt;
}

std::string MemoryOrchestrator::NextEpisodeId() {
    return "ep_" + session_id_ + "_" + std::to_string(next_episode_sequence_++);
}

absl::Status MemoryOrchestrator::QueueMidTermAnalysis(const Conversation& conversation_snapshot) {
    if (mid_term_compactor_ == nullptr || mid_term_flush_decider_ == nullptr) {
        return absl::OkStatus();
    }

    MidTermFlushDeciderPtr decider = mid_term_flush_decider_;
    MidTermCompactorPtr compactor = mid_term_compactor_;
    const std::string session_id = session_id_;
    const std::string episode_id = NextEpisodeId();
    pending_mid_term_flushes_.push_back(PendingMidTermFlush{
        .conversation_item_index = std::nullopt,
        .future = std::async(
            std::launch::async,
            [decider = std::move(decider), compactor = std::move(compactor), session_id, episode_id,
             conversation_snapshot]() -> absl::StatusOr<AsyncMidTermFlushResult> {
                const absl::StatusOr<MidTermFlushDecision> decision =
                    decider->Decide(conversation_snapshot);
                if (!decision.ok()) {
                    return decision.status();
                }

                const absl::StatusOr<std::optional<FlushTarget>> target =
                    ChooseFlushConversationItem(conversation_snapshot, *decision, session_id);
                if (!target.ok()) {
                    return target.status();
                }
                if (!target->has_value()) {
                    return AsyncMidTermFlushResult{};
                }

                const FlushTarget& chosen = target->value();
                absl::StatusOr<OngoingEpisodeFlushCandidate> candidate =
                    chosen.split_at_message_index.has_value()
                        ? CaptureOngoingEpisodeForSplitFlush(conversation_snapshot,
                                                             chosen.conversation_item_index,
                                                             *chosen.split_at_message_index)
                        : CaptureOngoingEpisodeForFlush(conversation_snapshot,
                                                        chosen.conversation_item_index);
                if (!candidate.ok()) {
                    return candidate.status();
                }

                const absl::StatusOr<CompactedMidTermEpisode> compacted =
                    compactor->Compact(MidTermCompactionRequest{
                        .session_id = session_id,
                        .flush_candidate = *candidate,
                    });
                if (!compacted.ok()) {
                    return compacted.status();
                }
                if (compacted->tier2_summary.empty() || compacted->tier3_ref.empty()) {
                    LOG(WARNING) << "MemoryOrchestrator rejected invalid mid-term compactor "
                                    "output"
                                 << " session_id=" << SanitizeForLog(session_id)
                                 << " episode_id=" << SanitizeForLog(episode_id)
                                 << " detail='mid-term compactor must produce non-empty "
                                    "tier2 and tier3 content'";
                    return absl::InvalidArgumentError(
                        "mid-term compactor must produce non-empty tier2 and tier3 "
                        "content");
                }

                CompletedOngoingEpisodeFlush flush{
                    .conversation_item_index = chosen.conversation_item_index,
                    .episode =
                        Episode{
                            .episode_id = episode_id,
                            .tier1_detail = compacted->tier1_detail,
                            .tier2_summary = compacted->tier2_summary,
                            .tier3_ref = compacted->tier3_ref,
                            .tier3_keywords = compacted->tier3_keywords,
                            .salience = compacted->salience,
                            .embedding = compacted->embedding,
                            .created_at = candidate->ongoing_episode.messages.back().create_time,
                        },
                    .stub_timestamp = NowTimestamp(),
                    .split_at_message_index = chosen.split_at_message_index,
                };
                if (absl::Status status = ValidateMidTermEpisodeWrite(MidTermEpisodeWrite{
                        .session_id = session_id,
                        .source_conversation_item_index =
                            static_cast<std::int64_t>(flush.conversation_item_index),
                        .episode = flush.episode,
                    });
                    !status.ok()) {
                    return status;
                }
                return AsyncMidTermFlushResult{
                    .completed_flush = std::move(flush),
                    .captured_message_count = candidate->ongoing_episode.messages.size(),
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
    const std::string episode_id = NextEpisodeId();
    MidTermCompactorPtr compactor = mid_term_compactor_;
    pending_mid_term_flushes_.push_back(
        PendingMidTermFlush{
            .conversation_item_index = flush_candidate.conversation_item_index,
            .future = std::async(
                std::launch::async,
                [compactor = std::move(compactor), session_id, episode_id, split_at_message_index,
                 flush_candidate = OngoingEpisodeFlushCandidate(
                     flush_candidate)]() -> absl::StatusOr<AsyncMidTermFlushResult> {
                    const absl::StatusOr<CompactedMidTermEpisode> compacted =
                        compactor->Compact(MidTermCompactionRequest{
                            .session_id = session_id,
                            .flush_candidate = flush_candidate,
                        });
                    if (!compacted.ok()) {
                        return compacted.status();
                    }
                    if (compacted->tier2_summary.empty() || compacted->tier3_ref.empty()) {
                        LOG(WARNING)
                            << "MemoryOrchestrator rejected invalid mid-term compactor output"
                            << " session_id=" << SanitizeForLog(session_id)
                            << " episode_id=" << SanitizeForLog(episode_id)
                            << " detail='mid-term compactor must produce non-empty tier2 and tier3 "
                               "content'";
                        return absl::InvalidArgumentError(
                            "mid-term compactor must produce non-empty tier2 and tier3 content");
                    }
                    CompletedOngoingEpisodeFlush flush{
                        .conversation_item_index = flush_candidate.conversation_item_index,
                        .episode =
                            Episode{
                                .episode_id = episode_id,
                                .tier1_detail = compacted->tier1_detail,
                                .tier2_summary = compacted->tier2_summary,
                                .tier3_ref = compacted->tier3_ref,
                                .tier3_keywords = compacted->tier3_keywords,
                                .salience = compacted->salience,
                                .embedding = compacted->embedding,
                                .created_at =
                                    flush_candidate.ongoing_episode.messages.back().create_time,
                            },
                        .stub_timestamp = NowTimestamp(),
                        .split_at_message_index = split_at_message_index,
                    };
                    if (absl::Status status = ValidateMidTermEpisodeWrite(MidTermEpisodeWrite{
                            .session_id = session_id,
                            .source_conversation_item_index =
                                static_cast<std::int64_t>(flush.conversation_item_index),
                            .episode = flush.episode,
                        });
                        !status.ok()) {
                        return status;
                    }
                    return AsyncMidTermFlushResult{
                        .completed_flush = std::move(flush),
                        .captured_message_count = flush_candidate.ongoing_episode.messages.size(),
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
        if (!result->completed_flush.has_value()) {
            VLOG(1) << "MemoryOrchestrator drained completed async mid-term analysis with no flush"
                    << " session_id=" << SanitizeForLog(session_id_);
            continue;
        }

        CompletedOngoingEpisodeFlush completed_flush = std::move(*result->completed_flush);
        if (adjusted_conversation_item_index.has_value()) {
            completed_flush.conversation_item_index = *adjusted_conversation_item_index;
        }

        if (!completed_flush.split_at_message_index.has_value() &&
            result->captured_message_count > 0U &&
            completed_flush.conversation_item_index < memory_.conversation().items.size()) {
            const ConversationItem& live_item =
                memory_.conversation().items[completed_flush.conversation_item_index];
            if (live_item.type == ConversationItemType::OngoingEpisode &&
                live_item.ongoing_episode.has_value() &&
                live_item.ongoing_episode->messages.size() > result->captured_message_count) {
                const std::size_t split_at = result->captured_message_count;
                if (split_at < 2U) {
                    LOG(WARNING) << "MemoryOrchestrator could not rebase async full flush to split"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " conversation_item_index="
                                 << completed_flush.conversation_item_index
                                 << " captured_message_count=" << result->captured_message_count
                                 << " live_message_count="
                                 << live_item.ongoing_episode->messages.size()
                                 << " detail='captured message count is too small to form a "
                                    "completed split portion'";
                    return invalid_argument(
                        "completed async mid-term flush cannot rebase to a split with fewer than "
                        "2 completed messages");
                }
                if (split_at >= live_item.ongoing_episode->messages.size()) {
                    LOG(WARNING) << "MemoryOrchestrator could not rebase async full flush to split"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " conversation_item_index="
                                 << completed_flush.conversation_item_index
                                 << " captured_message_count=" << result->captured_message_count
                                 << " live_message_count="
                                 << live_item.ongoing_episode->messages.size()
                                 << " detail='rebased split point falls outside the live message "
                                    "range'";
                    return invalid_argument(
                        "completed async mid-term flush cannot rebase split outside the live "
                        "message range");
                }
                if (live_item.ongoing_episode->messages[split_at].role != MessageRole::User) {
                    LOG(WARNING) << "MemoryOrchestrator could not rebase async full flush to split"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " conversation_item_index="
                                 << completed_flush.conversation_item_index
                                 << " captured_message_count=" << result->captured_message_count
                                 << " live_message_count="
                                 << live_item.ongoing_episode->messages.size()
                                 << " split_at_message_index=" << split_at
                                 << " detail='first live message after the captured portion is "
                                    "not a user message'";
                    return invalid_argument(
                        "completed async mid-term flush cannot rebase split because the first new "
                        "live message is not a user message");
                }
                completed_flush.split_at_message_index = split_at;
                VLOG(1) << "MemoryOrchestrator rebased async full flush to split session_id="
                        << SanitizeForLog(session_id_)
                        << " conversation_item_index=" << completed_flush.conversation_item_index
                        << " split_at_message_index=" << split_at;
            }
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
        ++drained_count;
    }
    return drained_count;
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
        if (!pending_mid_term_flushes_.empty()) {
            VLOG(1) << "MemoryOrchestrator skipped async mid-term analysis queue because another "
                       "analysis or flush is already pending"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " pending_count=" << pending_mid_term_flushes_.size();
            return absl::OkStatus();
        }
        return QueueMidTermAnalysis(conversation);
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
    return absl::OkStatus();
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

} // namespace isla::server::memory
