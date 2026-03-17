#include "isla/server/memory/memory_orchestrator.hpp"

#include <chrono>
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

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

Timestamp NowTimestamp() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
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
            << " episode_stub_created_at=" << SanitizeForLog(FormatTimestamp(flush.stub_timestamp))
            << " detail='" << SanitizeForLog(status.message()) << "'";
        return status;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<RetrievedMemory>>
MemoryOrchestrator::RetrieveRelevantMemories(const Message& user_message) {
    static_cast<void>(user_message);
    // TODO: Query mid-term and long-term memory systems here before prompt rendering.
    return std::nullopt;
}

absl::StatusOr<std::optional<std::size_t>>
MemoryOrchestrator::MaybeChooseFlushConversationItem() const {
    if (mid_term_compactor_ == nullptr || mid_term_flush_decider_ == nullptr) {
        return std::nullopt;
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return std::nullopt;
    }

    const absl::StatusOr<MidTermFlushDecision> decision =
        mid_term_flush_decider_->Decide(conversation);
    if (!decision.ok()) {
        return decision.status();
    }
    if (!decision->should_flush) {
        if (decision->conversation_item_index.has_value()) {
            LOG(WARNING)
                << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                << " session_id=" << SanitizeForLog(session_id_)
                << " detail='flush decider returned a conversation item while should_flush was "
                   "false'";
            return invalid_argument("mid-term flush decider cannot return a conversation item when "
                                    "should_flush is false");
        }
        VLOG(1) << "MemoryOrchestrator flush decider chose not to flush session_id="
                << SanitizeForLog(session_id_);
        return std::nullopt;
    }
    if (!decision->conversation_item_index.has_value()) {
        LOG(WARNING) << "MemoryOrchestrator rejected invalid mid-term flush decider output"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " detail='flush decider requested a flush without a conversation item'";
        return invalid_argument(
            "mid-term flush decider must return a conversation item when should_flush is true");
    }
    if (*decision->conversation_item_index >= conversation.items.size()) {
        LOG(WARNING)
            << "MemoryOrchestrator rejected invalid mid-term flush decider output"
            << " session_id=" << SanitizeForLog(session_id_)
            << " conversation_item_index=" << *decision->conversation_item_index
            << " conversation_size=" << conversation.items.size()
            << " detail='flush decider returned a conversation item outside the conversation "
               "range'";
        return invalid_argument(
            "mid-term flush decider returned a conversation item outside the conversation range");
    }
    VLOG(1) << "MemoryOrchestrator flush decider chose a conversation item for flush session_id="
            << SanitizeForLog(session_id_)
            << " conversation_item_index=" << *decision->conversation_item_index;
    return decision->conversation_item_index;
}

absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>>
MemoryOrchestrator::MaybeCaptureFlushCandidate(const Message& assistant_message) {
    if (mid_term_compactor_ == nullptr) {
        return std::nullopt;
    }
    if (assistant_message.role != MessageRole::Assistant) {
        return invalid_argument("flush capture currently requires an assistant message");
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return std::nullopt;
    }

    if (const absl::StatusOr<std::optional<std::size_t>> decider_choice =
            MaybeChooseFlushConversationItem();
        !decider_choice.ok()) {
        return decider_choice.status();
    } else if (decider_choice->has_value()) {
        for (const PendingMidTermFlush& pending_flush : pending_mid_term_flushes_) {
            if (pending_flush.conversation_item_index == decider_choice->value()) {
                return std::nullopt;
            }
        }
        return memory_.CaptureOngoingEpisodeForFlush(decider_choice->value());
    } else if (mid_term_flush_decider_ != nullptr) {
        return std::nullopt;
    }

    const std::size_t conversation_item_index = conversation.items.size() - 1U;
    for (const PendingMidTermFlush& pending_flush : pending_mid_term_flushes_) {
        if (pending_flush.conversation_item_index == conversation_item_index) {
            return std::nullopt;
        }
    }

    const ConversationItem& item = conversation.items.back();
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return std::nullopt;
    }
    if (item.ongoing_episode->messages.size() < 2U) {
        return std::nullopt;
    }

    return memory_.CaptureOngoingEpisodeForFlush(conversation_item_index);
}

std::string MemoryOrchestrator::NextEpisodeId() {
    return "ep_" + session_id_ + "_" + std::to_string(next_episode_sequence_++);
}

absl::Status
MemoryOrchestrator::QueueMidTermFlush(const OngoingEpisodeFlushCandidate& flush_candidate) {
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
                [compactor = std::move(compactor), session_id, episode_id,
                 flush_candidate = OngoingEpisodeFlushCandidate(
                     flush_candidate)]() -> absl::StatusOr<CompletedOngoingEpisodeFlush> {
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
                    return flush;
                }),
        });
    VLOG(1) << "MemoryOrchestrator queued async mid-term flush session_id="
            << SanitizeForLog(session_id_)
            << " conversation_item_index=" << flush_candidate.conversation_item_index;
    return absl::OkStatus();
}

absl::StatusOr<std::size_t> MemoryOrchestrator::DrainCompletedMidTermCompactions() {
    std::size_t drained_count = 0;
    for (auto it = pending_mid_term_flushes_.begin(); it != pending_mid_term_flushes_.end();) {
        if (it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        absl::StatusOr<CompletedOngoingEpisodeFlush> completed_flush = it->future.get();
        it = pending_mid_term_flushes_.erase(it);
        if (!completed_flush.ok()) {
            LOG(WARNING) << "MemoryOrchestrator async mid-term flush failed session_id="
                         << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(completed_flush.status().message()) << "'";
            return completed_flush.status();
        }
        if (absl::Status apply_status = ApplyCompletedEpisodeFlush(*completed_flush);
            !apply_status.ok()) {
            return apply_status;
        }
        VLOG(1) << "MemoryOrchestrator drained completed async mid-term flush session_id="
                << SanitizeForLog(session_id_)
                << " episode_id=" << SanitizeForLog(completed_flush->episode.episode_id)
                << " conversation_item_index=" << completed_flush->conversation_item_index;
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
        if (pending_flush.conversation_item_index == last_conversation_item_index) {
            VLOG(1) << "MemoryOrchestrator started a new ongoing episode before append because the "
                       "tail conversation item is still flushing"
                    << " session_id=" << SanitizeForLog(session_id_)
                    << " pending_conversation_item_index=" << pending_flush.conversation_item_index;
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
    const absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>> flush_candidate =
        MaybeCaptureFlushCandidate(assistant_message);
    if (!flush_candidate.ok()) {
        return flush_candidate.status();
    }
    if (flush_candidate->has_value()) {
        if (absl::Status queue_status = QueueMidTermFlush(flush_candidate->value());
            !queue_status.ok()) {
            return queue_status;
        }
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
    const absl::StatusOr<OngoingEpisodeFlushCandidate> captured =
        memory_.CaptureOngoingEpisodeForFlush(flush.conversation_item_index);
    if (!captured.ok()) {
        return captured.status();
    }
    if (absl::Status persistence_status = PersistCompletedEpisodeFlush(flush);
        !persistence_status.ok()) {
        return persistence_status;
    }
    return memory_.ApplyCompletedOngoingEpisodeFlush(flush);
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
