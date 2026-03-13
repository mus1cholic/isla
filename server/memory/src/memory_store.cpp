#include "isla/server/memory/memory_store.hpp"

#include <string_view>

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
            break;
        case ConversationItemType::EpisodeStub:
            if (!item.episode_stub.has_value()) {
                return absl::InvalidArgumentError(
                    "ValidateMemoryStoreSnapshot requires an episode_stub payload when "
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

} // namespace isla::server::memory
