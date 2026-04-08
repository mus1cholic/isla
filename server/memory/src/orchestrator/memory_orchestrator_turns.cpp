#include "isla/server/memory/memory_orchestrator.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/memory/conversation.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

absl::Status MemoryOrchestrator::AfterUserQueryAppended(
    const Message& user_message,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
    NoteUserTurnAppended();
    absl::StatusOr<std::optional<RetrievedMemory>> retrieved_memory =
        RetrieveRelevantMemories(user_message, std::move(telemetry_context));
    if (!retrieved_memory.ok()) {
        return retrieved_memory.status();
    }
    memory_.SetRetrievedMemory(std::move(*retrieved_memory));
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::AfterAssistantReplyAppended(
    const Message& assistant_message,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
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
        if (absl::Status status = QueueMidTermAnalysis(conversation, std::move(telemetry_context));
            !status.ok()) {
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
    if (absl::Status queue_status =
            QueueMidTermFlush(*candidate, std::nullopt, std::move(telemetry_context));
        !queue_status.ok()) {
        return queue_status;
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::HandleConversationMessage(
    std::string_view session_id, std::string_view turn_id, std::string_view text,
    Timestamp create_time, MessageRole role,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
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
        if (absl::Status post_status =
                AfterUserQueryAppended(user_message, std::move(telemetry_context));
            !post_status.ok()) {
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
        if (absl::Status post_status =
                AfterAssistantReplyAppended(assistant_message, std::move(telemetry_context));
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
                                                        query.create_time, MessageRole::User,
                                                        query.telemetry_context);
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
                                     MessageRole::Assistant, reply.telemetry_context);
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
