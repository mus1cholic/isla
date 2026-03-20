#pragma once

#include <cstddef>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/mid_term_compactor.hpp"
#include "isla/server/memory/mid_term_flush_decider.hpp"
#include "isla/server/memory/working_memory.hpp"

namespace isla::server::memory {

struct GatewayTurnText {
    std::string session_id;
    std::string turn_id;
    std::string text;
    Timestamp create_time;
};

struct GatewayUserQuery : GatewayTurnText {
    GatewayUserQuery() = default;
    GatewayUserQuery(std::string session_id_in, std::string turn_id_in, std::string text_in,
                     Timestamp create_time_in)
        : GatewayTurnText{
              .session_id = std::move(session_id_in),
              .turn_id = std::move(turn_id_in),
              .text = std::move(text_in),
              .create_time = create_time_in,
          } {}
};

struct GatewayAssistantReply : GatewayTurnText {
    GatewayAssistantReply() = default;
    GatewayAssistantReply(std::string session_id_in, std::string turn_id_in, std::string text_in,
                          Timestamp create_time_in)
        : GatewayTurnText{
              .session_id = std::move(session_id_in),
              .turn_id = std::move(turn_id_in),
              .text = std::move(text_in),
              .create_time = create_time_in,
          } {}
};

struct UserQueryMemoryResult {
    std::string rendered_system_prompt;
    std::string rendered_working_memory_context;
    std::string rendered_working_memory;
};

struct MemoryOrchestratorInit {
    std::string user_id;
    MemoryStorePtr store;
    MidTermFlushDeciderPtr mid_term_flush_decider = nullptr;
    MidTermCompactorPtr mid_term_compactor = nullptr;
};

// Central entry point for gateway-delivered user turns. The gateway only forwards the raw user
// query; this handler is responsible for converting it into working-memory state changes and
// coordinating future mid/long-term memory hooks.
class MemoryOrchestrator {
  public:
    MemoryOrchestrator(std::string session_id, WorkingMemory memory, MemoryStorePtr store = nullptr,
                       MidTermFlushDeciderPtr mid_term_flush_decider = nullptr,
                       MidTermCompactorPtr mid_term_compactor = nullptr);

    // Builds a fresh orchestrator with empty working-memory conversation state for the session.
    // Persistence and async mid-term components are optional and can be attached up front.
    [[nodiscard]] static absl::StatusOr<MemoryOrchestrator>
    Create(std::string session_id, const MemoryOrchestratorInit& init);

    // Persists the session record once so later turn handling can safely write conversation state.
    // When a store is configured, callers should invoke this before handling any messages.
    [[nodiscard]] absl::Status BeginSession(Timestamp create_time);

    // Handles a gateway user turn end to end: drains completed async flushes, appends and
    // persists the user message, refreshes retrieved memory, then returns the rendered prompt
    // pieces the gateway needs for the next model call.
    [[nodiscard]] absl::StatusOr<UserQueryMemoryResult>
    HandleUserQuery(const GatewayUserQuery& query);

    // Handles an assistant turn by appending/persisting the reply and kicking off any eligible
    // mid-term analysis or compaction work.
    [[nodiscard]] absl::Status HandleAssistantReply(const GatewayAssistantReply& reply);

    // Persists a completed flush first, then applies it to working memory. Split flushes may shift
    // later conversation item indices, so pending async work is adjusted after success.
    [[nodiscard]] absl::Status
    ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);

    // Polls all pending async mid-term tasks, applies any completed flushes whose targets are still
    // valid, and returns how many flushes were committed during this pass.
    [[nodiscard]] absl::StatusOr<std::size_t> DrainCompletedMidTermCompactions();

    // Returns whether any async mid-term analysis or compaction work is still pending.
    [[nodiscard]] bool HasPendingMidTermCompactions() const;

    // Renders only the system-prompt portion of the orchestrator's current working memory.
    [[nodiscard]] absl::StatusOr<std::string> RenderSystemPrompt() const;

    // Renders only the non-system-prompt context section of the current working memory.
    [[nodiscard]] absl::StatusOr<std::string> RenderWorkingMemoryContext() const;

    // Renders the full prompt bundle as a single string.
    [[nodiscard]] absl::StatusOr<std::string> RenderFullWorkingMemory() const;

    // Returns full Tier 1 detail for one expandable mid-term episode currently known to the
    // session. Missing episodes return NotFound; non-expandable episodes return FailedPrecondition.
    [[nodiscard]] absl::StatusOr<std::string>
    ExpandMidTermEpisode(std::string_view episode_id) const;

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    [[nodiscard]] const WorkingMemory& memory() const {
        return memory_;
    }

    [[nodiscard]] WorkingMemory& mutable_memory() {
        return memory_;
    }

  private:
    // Verifies that a gateway turn targets this session and includes the ids required for
    // persistence and logging.
    [[nodiscard]] absl::Status ValidateTurnText(std::string_view session_id,
                                                std::string_view turn_id,
                                                std::string_view role_label) const;

    // Guards persistence paths that require BeginSession to have succeeded first.
    [[nodiscard]] absl::Status ValidateSessionReadyForPersistence() const;

    // Writes the session row once and becomes a no-op after the session is marked persisted.
    [[nodiscard]] absl::Status PersistSessionIfNeeded(Timestamp create_time);

    // Persists the most recently appended conversation message from the live tail episode.
    [[nodiscard]] absl::Status PersistConversationMessage(std::string_view turn_id,
                                                          const Message& message);

    // Persists the flushed mid-term episode and its corresponding conversation-item mutation. For
    // split flushes, the remaining tail messages are read from live state so reloads stay exact.
    [[nodiscard]] absl::Status
    PersistCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);

    // Shared turn-ingest path used by both user and assistant messages.
    [[nodiscard]] absl::Status HandleConversationMessage(std::string_view session_id,
                                                         std::string_view turn_id,
                                                         std::string_view text,
                                                         Timestamp create_time, MessageRole role);

    // Freezes the current tail into its own conversation item before another append when a pending
    // flush still targets that tail item.
    void PrepareConversationForAppend();

    // Post-user hook for retrieval and other query-time memory enrichment.
    [[nodiscard]] absl::Status AfterUserQueryAppended(const Message& user_message);

    // Post-assistant hook for queueing mid-term analysis or direct compaction after a full
    // user/assistant exchange exists.
    [[nodiscard]] absl::Status AfterAssistantReplyAppended(const Message& assistant_message);

    // Retrieves extra memory to inject for the next prompt render. Today this is a placeholder and
    // may legitimately return no extra memory.
    [[nodiscard]] absl::StatusOr<std::optional<RetrievedMemory>>
    RetrieveRelevantMemories(const Message& user_message);

    // Runs the decider against a snapshot, and if it chooses a target, chains compaction work for
    // that captured conversation state off-thread.
    [[nodiscard]] absl::Status QueueMidTermAnalysis(const Conversation& conversation_snapshot);

    // Queues compaction for an already chosen flush candidate. The captured snapshot is owned by
    // the async task so later live conversation edits do not affect the compactor input.
    [[nodiscard]] absl::Status
    QueueMidTermFlush(const OngoingEpisodeFlushCandidate& flush_candidate,
                      std::optional<std::size_t> split_at_message_index = std::nullopt);

    // Produces stable per-session episode ids in the order completed flushes are applied.
    [[nodiscard]] std::string NextEpisodeId();

    struct CompletedFlushBuildInput {
        CompactedMidTermEpisode compacted;
        Timestamp episode_created_at;
        Timestamp stub_timestamp;
        std::optional<std::size_t> split_at_message_index;
    };

    struct AsyncMidTermFlushResult {
        std::optional<CompletedFlushBuildInput> completed_flush;
        std::size_t captured_message_count = 0;
    };

    struct PendingMidTermFlush {
        // Owned and polled only by the orchestrator thread. The future's async task may run on a
        // worker thread, but pending_mid_term_flushes_ itself is not shared concurrently.
        std::optional<std::size_t> conversation_item_index;
        std::future<absl::StatusOr<AsyncMidTermFlushResult>> future;
        bool freeze_tail_before_append = false;
    };

    std::string session_id_;
    WorkingMemory memory_;
    MemoryStorePtr store_;
    MidTermFlushDeciderPtr mid_term_flush_decider_;
    MidTermCompactorPtr mid_term_compactor_;
    std::vector<PendingMidTermFlush> pending_mid_term_flushes_;
    std::size_t next_episode_sequence_ = 1;
    bool session_persisted_ = false;
};

} // namespace isla::server::memory
