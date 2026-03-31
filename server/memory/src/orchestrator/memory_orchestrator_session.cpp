#include "isla/server/memory/memory_orchestrator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

absl::Status MemoryOrchestrator::BeginSession(Timestamp create_time) {
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

absl::StatusOr<std::optional<RetrievedMemory>>
MemoryOrchestrator::RetrieveRelevantMemories(const Message& /*user_message*/) {
    if (!store_) {
        return std::nullopt;
    }

    const PersistentMemoryCache& cache = memory_.snapshot().system_prompt.persistent_memory_cache;
    if (cache.active_models.empty() && cache.familiar_labels.empty()) {
        return std::nullopt;
    }

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
        } else {
            for (const Relationship& rel : *relationships) {
                context.append("- ");
                context.append(rel.from_entity_id);
                context.append(" ");
                context.append(rel.predicate);
                context.append(" ");
                context.append(rel.to_entity_id);
                context.push_back('\n');
            }
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

} // namespace isla::server::memory
