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
        return absl::InvalidArgumentError("memory session record must include a session_id");
    }
    if (record.user_id.empty()) {
        return absl::InvalidArgumentError("memory session record must include a user_id");
    }
    return absl::OkStatus();
}

absl::Status ValidateConversationMessageWrite(const ConversationMessageWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError("conversation message write must include a session_id");
    }
    if (write.conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "conversation message write item index must be non-negative");
    }
    if (write.message_index < 0) {
        return absl::InvalidArgumentError(
            "conversation message write message index must be non-negative");
    }
    if (write.turn_id.empty()) {
        return absl::InvalidArgumentError("conversation message write must include a turn_id");
    }
    if (write.content.empty()) {
        return absl::InvalidArgumentError("conversation message write content must be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateEpisodeStubWrite(const EpisodeStubWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError("episode stub write must include a session_id");
    }
    if (write.conversation_item_index < 0) {
        return absl::InvalidArgumentError("episode stub write item index must be non-negative");
    }
    if (write.episode_id.empty()) {
        return absl::InvalidArgumentError("episode stub write must include an episode_id");
    }
    if (write.episode_stub_content.empty()) {
        return absl::InvalidArgumentError("episode stub write content must be non-empty");
    }
    return absl::OkStatus();
}

absl::Status ValidateMidTermEpisodeWrite(const MidTermEpisodeWrite& write) {
    if (write.session_id.empty()) {
        return absl::InvalidArgumentError("mid-term episode write must include a session_id");
    }
    if (write.source_conversation_item_index < 0) {
        return absl::InvalidArgumentError(
            "mid-term episode source item index must be non-negative");
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
                "memory store snapshot conversation item indexes must be contiguous from zero");
        }
        switch (item.type) {
        case ConversationItemType::OngoingEpisode:
            if (!item.ongoing_episode.has_value()) {
                return absl::InvalidArgumentError(
                    "memory store snapshot ongoing episode item is missing its payload");
            }
            break;
        case ConversationItemType::EpisodeStub:
            if (!item.episode_stub.has_value()) {
                return absl::InvalidArgumentError(
                    "memory store snapshot episode stub item is missing its payload");
            }
            if (!item.episode_id.has_value() || item.episode_id->empty()) {
                return absl::InvalidArgumentError(
                    "memory store snapshot episode stub item must reference an episode_id");
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
                "memory store snapshot mid-term episodes must be ordered by created_at");
        }
        previous_episode_time = episode.created_at;
    }

    return absl::OkStatus();
}

} // namespace isla::server::memory
