#include "isla/server/memory/mid_term_memory.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"

namespace isla::server::memory {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

absl::Status ValidateEpisodeId(std::string_view episode_id) {
    if (episode_id.empty()) {
        return invalid_argument("mid-term memory requires episode_id to be non-empty");
    }
    return absl::OkStatus();
}

} // namespace

MidTermMemory::MidTermMemory(std::string session_id, MemoryStorePtr store)
    : session_id_(std::move(session_id)), store_(std::move(store)) {}

absl::StatusOr<MidTermMemory> MidTermMemory::Create(const MidTermMemoryInit& init) {
    if (init.session_id.empty()) {
        return invalid_argument("mid-term memory requires a non-empty session_id");
    }
    if (init.store == nullptr) {
        return failed_precondition("mid-term memory requires a configured MemoryStore");
    }

    return MidTermMemory(init.session_id, init.store);
}

absl::Status MidTermMemory::StoreEpisode(std::int64_t source_conversation_item_index,
                                         const Episode& episode) {
    if (store_ == nullptr) {
        return failed_precondition("mid-term memory is missing its MemoryStore");
    }

    const MidTermEpisodeWrite write{
        .session_id = session_id_,
        .source_conversation_item_index = source_conversation_item_index,
        .episode = episode,
    };
    if (absl::Status status = ValidateMidTermEpisodeWrite(write); !status.ok()) {
        return status;
    }

    absl::Status status = store_->UpsertMidTermEpisode(write);
    if (!status.ok()) {
        LOG(WARNING) << "MidTermMemory failed to store episode"
                     << " session_id=" << session_id_ << " episode_id=" << episode.episode_id
                     << " source_conversation_item_index=" << source_conversation_item_index
                     << " detail='" << status.message() << "'";
        return status;
    }

    LOG(INFO) << "MidTermMemory stored episode"
              << " session_id=" << session_id_ << " episode_id=" << episode.episode_id
              << " source_conversation_item_index=" << source_conversation_item_index
              << " salience=" << episode.salience
              << " expandable=" << (IsExpandableEpisode(episode) ? "true" : "false");
    return absl::OkStatus();
}

absl::StatusOr<std::vector<Episode>> MidTermMemory::ListEpisodes() const {
    if (store_ == nullptr) {
        return failed_precondition("mid-term memory is missing its MemoryStore");
    }
    absl::StatusOr<std::vector<Episode>> episodes = store_->ListMidTermEpisodes(session_id_);
    if (!episodes.ok()) {
        LOG(WARNING) << "MidTermMemory failed to list episodes"
                     << " session_id=" << session_id_ << " detail='" << episodes.status().message()
                     << "'";
        return episodes.status();
    }
    return episodes;
}

absl::StatusOr<std::optional<Episode>>
MidTermMemory::FindEpisode(std::string_view episode_id) const {
    if (store_ == nullptr) {
        return failed_precondition("mid-term memory is missing its MemoryStore");
    }
    if (absl::Status status = ValidateEpisodeId(episode_id); !status.ok()) {
        return status;
    }
    absl::StatusOr<std::optional<Episode>> episode =
        store_->GetMidTermEpisode(session_id_, episode_id);
    if (!episode.ok()) {
        LOG(WARNING) << "MidTermMemory failed to fetch episode"
                     << " session_id=" << session_id_ << " episode_id=" << episode_id << " detail='"
                     << episode.status().message() << "'";
        return episode.status();
    }
    return episode;
}

absl::StatusOr<Episode> MidTermMemory::GetRequiredEpisode(std::string_view episode_id) const {
    const absl::StatusOr<std::optional<Episode>> episode = FindEpisode(episode_id);
    if (!episode.ok()) {
        return episode.status();
    }
    if (!episode->has_value()) {
        return absl::NotFoundError("mid-term episode was not found");
    }
    return episode->value();
}

absl::StatusOr<bool> MidTermMemory::HasExpandableDetail(std::string_view episode_id) const {
    const absl::StatusOr<Episode> episode = GetRequiredEpisode(episode_id);
    if (!episode.ok()) {
        return episode.status();
    }
    return IsExpandableEpisode(*episode);
}

absl::StatusOr<std::string> MidTermMemory::GetExpandableDetail(std::string_view episode_id) const {
    const absl::StatusOr<Episode> episode = GetRequiredEpisode(episode_id);
    if (!episode.ok()) {
        return episode.status();
    }
    if (!IsExpandableEpisode(*episode)) {
        return failed_precondition(
            "mid-term episode does not have expandable Tier 1 detail available");
    }
    VLOG(1) << "MidTermMemory served expandable detail"
            << " session_id=" << session_id_ << " episode_id=" << episode_id;
    return *episode->tier1_detail;
}

} // namespace isla::server::memory
