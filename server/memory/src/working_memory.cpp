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

absl::Status WorkingMemory::FlushOngoingEpisode(const FlushRequest& request) {
    if (request.episode.episode_id.empty() || request.episode.tier2_summary.empty() ||
        request.episode.tier3_ref.empty()) {
        return invalid_argument("flush episode must include id and summary fields");
    }
    const absl::Status replace_status =
        ReplaceOngoingEpisodeWithStub(state_.conversation, request.conversation_item_index,
                                      request.stub_text, request.stub_timestamp);
    if (!replace_status.ok()) {
        LOG(WARNING) << "WorkingMemory flush failed conversation_item_index="
                     << request.conversation_item_index << " detail='" << replace_status.message()
                     << "'";
        return replace_status;
    }

    auto insert_at = std::upper_bound(state_.mid_term_episodes.begin(),
                                      state_.mid_term_episodes.end(), request.episode.created_at,
                                      [](Timestamp created_at, const Episode& episode) {
                                          return created_at < episode.created_at;
                                      });
    state_.mid_term_episodes.insert(insert_at, request.episode);
    LOG(INFO) << "WorkingMemory flushed ongoing episode conversation_item_index="
              << request.conversation_item_index << " episode_id=" << request.episode.episode_id
              << " mid_term_count=" << state_.mid_term_episodes.size();
    return absl::OkStatus();
}

std::string WorkingMemory::RenderPrompt() const {
    return RenderWorkingMemoryPrompt(state_);
}

} // namespace isla::server::memory
