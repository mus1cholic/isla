#include "isla/server/memory/memory_store.hpp"

#include <unordered_set>

#include "absl/status/status.h"

namespace isla::server::memory {
namespace {

absl::Status ValidateEpisodeCoreFields(const Episode& episode) {
    if (episode.episode_id.empty()) {
        return absl::InvalidArgumentError("mid-term episode write must include an episode_id");
    }
    if (episode.tier2_summary.empty()) {
        return absl::InvalidArgumentError("mid-term episode write must include a tier2_summary");
    }
    if (episode.tier3_ref.empty()) {
        return absl::InvalidArgumentError("mid-term episode write must include a tier3_ref");
    }
    if (episode.salience < 1 || episode.salience > 10) {
        return absl::InvalidArgumentError(
            "mid-term episode write salience must be in the range 1-10");
    }
    return absl::OkStatus();
}

} // namespace

absl::Status ValidateMemorySessionRecord(const MemorySessionRecord& record) {
    if (record.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateMemorySessionRecord requires session_id to be non-empty");
    }
    if (record.user_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateMemorySessionRecord requires user_id to be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateUserWorkingMemoryRecord(const UserWorkingMemoryRecord& record) {
    if (record.user_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateUserWorkingMemoryRecord requires user_id to be non-empty");
    }
    if (record.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateUserWorkingMemoryRecord requires session_id to be non-empty");
    }
    if (record.rendered_working_memory.empty()) {
        return absl::InvalidArgumentError(
            "ValidateUserWorkingMemoryRecord requires rendered_working_memory to be non-empty");
    }
    if (record.working_memory.conversation.user_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateUserWorkingMemoryRecord requires working_memory.conversation.user_id to be "
            "non-empty");
    }
    if (record.working_memory.conversation.user_id != record.user_id) {
        return absl::InvalidArgumentError(
            "ValidateUserWorkingMemoryRecord requires working_memory.conversation.user_id to "
            "match user_id");
    }
    return absl::OkStatus();
}

absl::Status ValidateConversationMessageWrite(const ConversationMessageWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateConversationMessageWrite requires session_id to be non-empty");
    }
    if (write.conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "ValidateConversationMessageWrite requires conversation_item_index to be non-negative");
    }
    if (write.message_index < 0) {
        return absl::InvalidArgumentError(
            "ValidateConversationMessageWrite requires message_index to be non-negative");
    }
    if (write.turn_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateConversationMessageWrite requires turn_id to be non-empty");
    }
    if (write.content.empty()) {
        return absl::InvalidArgumentError(
            "ValidateConversationMessageWrite requires content to be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateEpisodeStubWrite(const EpisodeStubWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateEpisodeStubWrite requires session_id to be non-empty");
    }
    if (write.conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "ValidateEpisodeStubWrite requires conversation_item_index to be non-negative");
    }
    if (write.episode_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateEpisodeStubWrite requires episode_id to be non-empty");
    }
    if (write.episode_stub_content.empty()) {
        return absl::InvalidArgumentError(
            "ValidateEpisodeStubWrite requires episode_stub_content to be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateSplitEpisodeStubWrite(const SplitEpisodeStubWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateSplitEpisodeStubWrite requires session_id to be non-empty");
    }
    if (write.conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "ValidateSplitEpisodeStubWrite requires conversation_item_index to be non-negative");
    }
    if (write.episode_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateSplitEpisodeStubWrite requires episode_id to be non-empty");
    }
    if (write.episode_stub_content.empty()) {
        return absl::InvalidArgumentError(
            "ValidateSplitEpisodeStubWrite requires episode_stub_content to be non-empty");
    }
    if (write.remaining_ongoing_episode.messages.empty()) {
        return absl::InvalidArgumentError(
            "ValidateSplitEpisodeStubWrite requires remaining_ongoing_episode to contain at least "
            "one message");
    }
    return absl::OkStatus();
}

absl::Status ValidateMidTermEpisodeWrite(const MidTermEpisodeWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateMidTermEpisodeWrite requires session_id to be non-empty");
    }
    if (write.source_conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "ValidateMidTermEpisodeWrite requires source_conversation_item_index to be "
            "non-negative");
    }
    return ValidateEpisodeCoreFields(write.episode);
}

absl::Status ValidateMemoryStoreSnapshot(const MemoryStoreSnapshot& snapshot) {
    if (absl::Status status = ValidateMemorySessionRecord(snapshot.session); !status.ok()) {
        return status;
    }

    std::int64_t expected_item_index = 0;
    for (const PersistedConversationItem& item : snapshot.conversation_items) {
        if (item.conversation_item_index != expected_item_index) {
            return absl::InvalidArgumentError(
                "ValidateMemoryStoreSnapshot requires conversation_items to use contiguous "
                "conversation_item_index values starting at zero");
        }
        switch (item.type) {
        case ConversationItemType::OngoingEpisode:
            if (!item.ongoing_episode.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires an ongoing_episode payload when "
                    "conversation_items.type is ongoing_episode");
            }
            if (item.episode_stub.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires episode_stub to be unset when "
                    "conversation_items.type is ongoing_episode");
            }
            if (item.episode_id.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires episode_id to be unset when "
                    "conversation_items.type is ongoing_episode");
            }
            break;
        case ConversationItemType::EpisodeStub:
            if (!item.episode_stub.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires an episode_stub payload when "
                    "conversation_items.type is episode_stub");
            }
            if (item.ongoing_episode.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires ongoing_episode to be unset when "
                    "conversation_items.type is episode_stub");
            }
            if (!item.episode_id.has_value() || item.episode_id->empty()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires episode_stub conversation items to "
                    "reference a non-empty episode_id");
            }
            break;
        }
        ++expected_item_index;
    }

    std::optional<Timestamp> previous_episode_time;
    for (const Episode& episode : snapshot.mid_term_episodes) {
        if (absl::Status status = ValidateEpisodeCoreFields(episode); !status.ok()) {
            return status;
        }
        if (previous_episode_time.has_value() && episode.created_at < *previous_episode_time) {
            return absl::InvalidArgumentError(
                "ValidateMemoryStoreSnapshot requires mid_term_episodes to be ordered by "
                "created_at");
        }
        previous_episode_time = episode.created_at;
    }

    return absl::OkStatus();
}

absl::Status ValidateEntityWrite(const EntityWrite& write) {
    const Entity& entity = write.entity;
    if (entity.entity_id.empty()) {
        return absl::InvalidArgumentError("ValidateEntityWrite requires entity_id to be non-empty");
    }
    if (entity.user_id.empty()) {
        return absl::InvalidArgumentError("ValidateEntityWrite requires user_id to be non-empty");
    }
    if (entity.label.empty()) {
        return absl::InvalidArgumentError("ValidateEntityWrite requires label to be non-empty");
    }
    if (entity.category.empty()) {
        return absl::InvalidArgumentError("ValidateEntityWrite requires category to be non-empty");
    }
    if (entity.activeness < 1 || entity.activeness > 10) {
        return absl::InvalidArgumentError(
            "ValidateEntityWrite requires activeness to be in the range 1-10");
    }
    return absl::OkStatus();
}

absl::Status ValidateRelationshipWrite(const RelationshipWrite& write) {
    const Relationship& rel = write.relationship;
    if (rel.relationship_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateRelationshipWrite requires relationship_id to be non-empty");
    }
    if (rel.user_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateRelationshipWrite requires user_id to be non-empty");
    }
    if (rel.from_entity_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateRelationshipWrite requires from_entity_id to be non-empty");
    }
    if (rel.predicate.empty()) {
        return absl::InvalidArgumentError(
            "ValidateRelationshipWrite requires predicate to be non-empty");
    }
    if (rel.to_entity_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateRelationshipWrite requires to_entity_id to be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateLongTermEpisodeWrite(const LongTermEpisodeWrite& write) {
    const LongTermEpisode& episode = write.episode;
    if (episode.lte_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeWrite requires lte_id to be non-empty");
    }
    if (episode.user_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeWrite requires user_id to be non-empty");
    }
    if (episode.summary_compressed.empty()) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeWrite requires summary_compressed to be non-empty");
    }
    if (episode.complexity < 1 || episode.complexity > 10) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeWrite requires complexity to be in the range 1-10");
    }
    return absl::OkStatus();
}

absl::Status ValidateLongTermEpisodeEntityLink(const LongTermEpisodeEntityLink& link) {
    if (link.lte_id.empty()) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeEntityLink requires lte_id to be non-empty");
    }
    if (link.entity_ids.empty()) {
        return absl::InvalidArgumentError(
            "ValidateLongTermEpisodeEntityLink requires at least one entity_id");
    }
    std::unordered_set<std::string> seen;
    for (const std::string& entity_id : link.entity_ids) {
        if (entity_id.empty()) {
            return absl::InvalidArgumentError(
                "ValidateLongTermEpisodeEntityLink requires all entity_ids to be non-empty");
        }
        if (!seen.insert(entity_id).second) {
            return absl::InvalidArgumentError(
                "ValidateLongTermEpisodeEntityLink requires all entity_ids to be unique");
        }
    }
    return absl::OkStatus();
}

} // namespace isla::server::memory
