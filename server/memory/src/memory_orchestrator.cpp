#include "isla/server/memory/memory_orchestrator.hpp"

#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/working_memory_utils.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

MemoryOrchestrator::MemoryOrchestrator(std::string session_id, WorkingMemory memory)
    : session_id_(std::move(session_id)), memory_(std::move(memory)) {}

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
    return MemoryOrchestrator(std::move(session_id), std::move(*memory));
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
        VLOG(1) << "MemoryOrchestrator identified a flush candidate session_id=" << session_id_
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

    if (role == MessageRole::User) {
        AppendUserMessage(memory_.mutable_conversation(), std::string(text), create_time);
        const Message& user_message =
            memory_.conversation().items.back().ongoing_episode->messages.back();
        if (absl::Status post_status = AfterUserQueryAppended(user_message); !post_status.ok()) {
            return post_status;
        }
    } else {
        AppendAssistantMessage(memory_.mutable_conversation(), std::string(text), create_time);
        const Message& assistant_message =
            memory_.conversation().items.back().ongoing_episode->messages.back();
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

    absl::StatusOr<std::string> rendered_working_memory = RenderFullWorkingMemory();
    if (!rendered_working_memory.ok()) {
        return rendered_working_memory.status();
    }

    return UserQueryMemoryResult{ .rendered_working_memory = std::move(*rendered_working_memory) };
}

absl::Status MemoryOrchestrator::HandleAssistantReply(const GatewayAssistantReply& reply) {
    return HandleConversationMessage(reply.session_id, reply.turn_id, reply.text, reply.create_time,
                                     MessageRole::Assistant);
}

absl::Status
MemoryOrchestrator::ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush) {
    return memory_.ApplyCompletedOngoingEpisodeFlush(flush);
}

absl::StatusOr<std::string> MemoryOrchestrator::RenderFullWorkingMemory() const {
    return memory_.RenderFullWorkingMemory();
}

} // namespace isla::server::memory
