#include "isla/server/memory/working_memory.hpp"

#include <algorithm>
#include <utility>

#include "absl/log/log.h"
#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/working_memory_utils.hpp"

namespace isla::server::memory {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

bool IsExpandableEpisode(const Episode& episode) {
    return episode.salience >= 8 && episode.tier1_detail.has_value() &&
           !episode.tier1_detail->empty();
}

absl::Status ValidateCompletedEpisode(const Episode& episode) {
    if (episode.episode_id.empty() || episode.tier2_summary.empty() || episode.tier3_ref.empty()) {
        return invalid_argument("completed flush episode must include id, tier2, and tier3 data");
    }
    return absl::OkStatus();
}

WorkingMemory::WorkingMemory(WorkingMemoryState state) : state_(std::move(state)) {}

WorkingMemory WorkingMemory::Create(const WorkingMemoryInit& init) {
    return WorkingMemory(WorkingMemoryState{
        .system_prompt = init.system_prompt,
        .persistent_memory_cache = {},
        .mid_term_episodes = {},
        .retrieved_memory = std::nullopt,
        .conversation =
            Conversation{
                .items = {},
                .user_id = init.user_id,
            },
    });
}

void WorkingMemory::AppendEpisodeStub(std::string content, Timestamp create_time) {
    isla::server::memory::AppendEpisodeStub(state_.conversation, std::move(content), create_time);
}

void WorkingMemory::SetRetrievedMemory(std::optional<RetrievedMemory> retrieved_memory) {
    state_.retrieved_memory = std::move(retrieved_memory);
    VLOG(1) << "WorkingMemory updated retrieved memory present="
            << (state_.retrieved_memory.has_value() ? "true" : "false");
}

void WorkingMemory::UpsertActiveModel(std::string entity_id, std::string text) {
    const std::string entity_id_for_log = entity_id;
    isla::server::memory::UpsertActiveModel(state_.persistent_memory_cache, std::move(entity_id),
                                            std::move(text));
    LOG(INFO) << "WorkingMemory upserted active model entity_id=" << entity_id_for_log
              << " active_count=" << state_.persistent_memory_cache.active_models.size();
}

void WorkingMemory::UpsertFamiliarLabel(std::string entity_id, std::string text) {
    const std::string entity_id_for_log = entity_id;
    isla::server::memory::UpsertFamiliarLabel(state_.persistent_memory_cache, std::move(entity_id),
                                              std::move(text));
    LOG(INFO) << "WorkingMemory upserted familiar label entity_id=" << entity_id_for_log
              << " familiar_count=" << state_.persistent_memory_cache.familiar_labels.size();
}

absl::Status WorkingMemory::WriteBackCoreEntity(std::string_view entity_id, std::string text) {
    if (entity_id != kUserEntityId && entity_id != kAssistantEntityId) {
        LOG(WARNING) << "WorkingMemory write-back rejected for non-core entity entity_id="
                     << entity_id;
        return invalid_argument("write-back caching is limited to hardcoded core entities");
    }

    UpsertActiveModel(std::string(entity_id), std::move(text));
    LOG(INFO) << "WorkingMemory write-back applied for core entity entity_id=" << entity_id;
    return absl::OkStatus();
}

absl::StatusOr<OngoingEpisodeFlushCandidate>
WorkingMemory::CaptureOngoingEpisodeForFlush(std::size_t conversation_item_index) const {
    if (conversation_item_index >= state_.conversation.items.size()) {
        return invalid_argument("flush target exceeds conversation size");
    }
    const auto& item = state_.conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        return invalid_argument("flush target must be an ongoing episode");
    }
    if (item.ongoing_episode->messages.empty()) {
        return invalid_argument("flush target must contain at least one message");
    }

    VLOG(1) << "WorkingMemory captured ongoing episode for async flush conversation_item_index="
            << conversation_item_index
            << " message_count=" << item.ongoing_episode->messages.size();
    // TODO: The async flush worker should consume this snapshot to perform natural-boundary
    // detection and generate tier1/tier2/tier3 before ApplyCompletedOngoingEpisodeFlush mutates
    // conversation state.
    return OngoingEpisodeFlushCandidate{
        .conversation_item_index = conversation_item_index,
        .ongoing_episode = *item.ongoing_episode,
    };
}

absl::Status
WorkingMemory::ApplyCompletedOngoingEpisodeFlush(const CompletedOngoingEpisodeFlush& flush) {
    const absl::Status episode_status = ValidateCompletedEpisode(flush.episode);
    if (!episode_status.ok()) {
        return episode_status;
    }

    const absl::Status replace_status =
        ReplaceOngoingEpisodeWithStub(state_.conversation, flush.conversation_item_index,
                                      flush.episode.tier3_ref, flush.stub_timestamp);
    if (!replace_status.ok()) {
        LOG(WARNING) << "WorkingMemory apply flush failed conversation_item_index="
                     << flush.conversation_item_index << " detail='" << replace_status.message()
                     << "'";
        return replace_status;
    }

    auto insert_at = std::upper_bound(state_.mid_term_episodes.begin(),
                                      state_.mid_term_episodes.end(), flush.episode.created_at,
                                      [](Timestamp created_at, const Episode& episode) {
                                          return created_at < episode.created_at;
                                      });
    state_.mid_term_episodes.insert(insert_at, flush.episode);
    LOG(INFO) << "WorkingMemory applied completed flush conversation_item_index="
              << flush.conversation_item_index << " episode_id=" << flush.episode.episode_id
              << " mid_term_count=" << state_.mid_term_episodes.size();
    return absl::OkStatus();
}

absl::StatusOr<std::string> WorkingMemory::RenderFullWorkingMemory() const {
    return RenderWorkingMemoryPrompt(state_);
}

} // namespace isla::server::memory
