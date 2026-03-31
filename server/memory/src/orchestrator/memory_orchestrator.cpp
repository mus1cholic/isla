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
    std::size_t mid_term_flush_decider_interval_user_turns)
    : session_id_(std::move(session_id)), memory_(std::move(memory)), store_(std::move(store)),
      mid_term_flush_decider_(std::move(mid_term_flush_decider)),
      mid_term_compactor_(std::move(mid_term_compactor)),
      sleep_cycle_semantic_extractor_(std::move(sleep_cycle_semantic_extractor)),
      pending_mid_term_flushes_(),
      mid_term_flush_decider_interval_user_turns_(mid_term_flush_decider_interval_user_turns),
      user_turns_since_last_mid_term_decider_run_(0), next_episode_sequence_(1),
      session_persisted_(false) {
    CHECK_GT(mid_term_flush_decider_interval_user_turns_, 0U)
        << "mid_term_flush_decider_interval_user_turns must be at least 1";
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

    absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
        .system_prompt = "",
        .user_id = init.user_id,
    });
    if (!memory.ok()) {
        return memory.status();
    }
    return MemoryOrchestrator(std::move(session_id), std::move(*memory), init.store,
                              init.mid_term_flush_decider, init.mid_term_compactor,
                              init.sleep_cycle_semantic_extractor,
                              init.mid_term_flush_decider_interval_user_turns);
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
