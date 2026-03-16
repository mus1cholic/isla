#include "isla/server/memory/memory_orchestrator.hpp"

#include <cstdint>
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

} // namespace

MemoryOrchestrator::MemoryOrchestrator(std::string session_id, WorkingMemory memory,
                                       MemoryStorePtr store)
    : session_id_(std::move(session_id)), memory_(std::move(memory)), store_(std::move(store)),
      session_persisted_(false) {}

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
    return MemoryOrchestrator(std::move(session_id), std::move(*memory), init.store);
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

absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>>
MemoryOrchestrator::MaybeCaptureFlushCandidate(const Message& user_message) {
    static_cast<void>(user_message);
    // TODO: Trigger semantic-boundary/threshold-based flush capture here.
    return std::nullopt;
}

absl::Status MemoryOrchestrator::AfterUserQueryAppended(const Message& user_message) {
    absl::StatusOr<std::optional<RetrievedMemory>> retrieved_memory =
        RetrieveRelevantMemories(user_message);
    if (!retrieved_memory.ok()) {
        return retrieved_memory.status();
    }
    memory_.SetRetrievedMemory(std::move(*retrieved_memory));

    const absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>> flush_candidate =
        MaybeCaptureFlushCandidate(user_message);
    if (!flush_candidate.ok()) {
        return flush_candidate.status();
    }
    if (flush_candidate->has_value()) {
        // TODO: Queue async flush work when a candidate is returned.
        VLOG(1) << "MemoryOrchestrator identified a flush candidate session_id="
                << SanitizeForLog(session_id_)
                << " conversation_item_index=" << flush_candidate->value().conversation_item_index;
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::AfterAssistantReplyAppended(const Message& assistant_message) {
    static_cast<void>(assistant_message);
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
