#include "isla/server/memory/memory_orchestrator.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"

namespace isla::server::memory {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

MemoryOrchestrator::MemoryOrchestrator(
    std::string session_id, WorkingMemory memory, MemoryStorePtr store,
    MidTermFlushDeciderPtr mid_term_flush_decider, MidTermCompactorPtr mid_term_compactor,
    SleepCycleSemanticExtractorPtr sleep_cycle_semantic_extractor,
    std::size_t mid_term_flush_decider_interval_user_turns,
    RetrievedMemoryRerankerPtr retrieved_memory_reranker,
    double retrieved_memory_reranker_min_score,
    std::shared_ptr<const isla::server::EmbeddingClient> retrieval_embedding_client,
    std::string retrieval_embedding_model, int episode_similarity_search_top_k,
    int kg_hop_1_top_k_per_entity, int kg_hop_2_top_k_per_entity)
    : session_id_(std::move(session_id)), memory_(std::move(memory)), store_(std::move(store)),
      mid_term_flush_decider_(std::move(mid_term_flush_decider)),
      mid_term_compactor_(std::move(mid_term_compactor)),
      sleep_cycle_semantic_extractor_(std::move(sleep_cycle_semantic_extractor)),
      retrieved_memory_reranker_(std::move(retrieved_memory_reranker)),
      retrieval_embedding_client_(std::move(retrieval_embedding_client)),
      retrieval_embedding_model_(std::move(retrieval_embedding_model)), pending_mid_term_flushes_(),
      mid_term_flush_decider_interval_user_turns_(mid_term_flush_decider_interval_user_turns),
      user_turns_since_last_mid_term_decider_run_(0), next_episode_sequence_(1),
      retrieved_memory_reranker_min_score_(retrieved_memory_reranker_min_score),
      episode_similarity_search_top_k_(episode_similarity_search_top_k),
      kg_hop_1_top_k_per_entity_(kg_hop_1_top_k_per_entity),
      kg_hop_2_top_k_per_entity_(kg_hop_2_top_k_per_entity), session_persisted_(false) {
    CHECK_GT(mid_term_flush_decider_interval_user_turns_, 0U)
        << "mid_term_flush_decider_interval_user_turns must be at least 1";
    CHECK_GT(episode_similarity_search_top_k_, 0)
        << "episode_similarity_search_top_k must be at least 1";
    CHECK_GT(kg_hop_1_top_k_per_entity_, 0) << "kg_hop_1_top_k_per_entity must be at least 1";
    CHECK_GT(kg_hop_2_top_k_per_entity_, 0) << "kg_hop_2_top_k_per_entity must be at least 1";
}

absl::StatusOr<MemoryOrchestrator> MemoryOrchestrator::Create(std::string session_id,
                                                              const MemoryOrchestratorInit& init) {
    if (session_id.empty()) {
        return invalid_argument("memory orchestrator must include a session_id");
    }
    if (init.user_id.empty()) {
        return invalid_argument("memory orchestrator must include a user_id");
    }
    if (init.mid_term_flush_decider_interval_user_turns == 0U) {
        return invalid_argument(
            "memory orchestrator mid-term flush decider interval must be at least 1 user turn");
    }
    if (init.episode_similarity_search_top_k <= 0) {
        return invalid_argument(
            "memory orchestrator episode_similarity_search_top_k must be at least 1");
    }
    if (init.kg_hop_1_top_k_per_entity <= 0) {
        return invalid_argument("memory orchestrator kg_hop_1_top_k_per_entity must be at least 1");
    }
    if (init.kg_hop_2_top_k_per_entity <= 0) {
        return invalid_argument("memory orchestrator kg_hop_2_top_k_per_entity must be at least 1");
    }

    absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
        .system_prompt = "",
        .user_id = init.user_id,
    });
    if (!memory.ok()) {
        return memory.status();
    }
    return MemoryOrchestrator(
        std::move(session_id), std::move(*memory), init.store, init.mid_term_flush_decider,
        init.mid_term_compactor, init.sleep_cycle_semantic_extractor,
        init.mid_term_flush_decider_interval_user_turns, init.retrieved_memory_reranker,
        init.retrieved_memory_reranker_min_score, init.retrieval_embedding_client,
        init.retrieval_embedding_model, init.episode_similarity_search_top_k,
        init.kg_hop_1_top_k_per_entity, init.kg_hop_2_top_k_per_entity);
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
