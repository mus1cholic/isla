#pragma once

#include <future>
#include <optional>
#include <string>
#include <string_view>
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

    [[nodiscard]] static absl::StatusOr<MemoryOrchestrator>
    Create(std::string session_id, const MemoryOrchestratorInit& init);

    [[nodiscard]] absl::Status BeginSession(Timestamp create_time);
    [[nodiscard]] absl::StatusOr<UserQueryMemoryResult>
    HandleUserQuery(const GatewayUserQuery& query);
    [[nodiscard]] absl::Status HandleAssistantReply(const GatewayAssistantReply& reply);
    [[nodiscard]] absl::Status
    ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);
    [[nodiscard]] absl::StatusOr<std::size_t> DrainCompletedMidTermCompactions();
    [[nodiscard]] absl::StatusOr<std::string> RenderSystemPrompt() const;
    [[nodiscard]] absl::StatusOr<std::string> RenderWorkingMemoryContext() const;
    [[nodiscard]] absl::StatusOr<std::string> RenderFullWorkingMemory() const;

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
    [[nodiscard]] absl::Status ValidateTurnText(std::string_view session_id,
                                                std::string_view turn_id,
                                                std::string_view role_label) const;
    [[nodiscard]] absl::Status ValidateSessionReadyForPersistence() const;
    [[nodiscard]] absl::Status PersistSessionIfNeeded(Timestamp create_time);
    [[nodiscard]] absl::Status PersistConversationMessage(std::string_view turn_id,
                                                          const Message& message);
    [[nodiscard]] absl::Status
    PersistCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);
    [[nodiscard]] absl::Status HandleConversationMessage(std::string_view session_id,
                                                         std::string_view turn_id,
                                                         std::string_view text,
                                                         Timestamp create_time, MessageRole role);
    void PrepareConversationForAppend();
    [[nodiscard]] absl::Status AfterUserQueryAppended(const Message& user_message);
    [[nodiscard]] absl::Status AfterAssistantReplyAppended(const Message& assistant_message);
    [[nodiscard]] absl::StatusOr<std::optional<RetrievedMemory>>
    RetrieveRelevantMemories(const Message& user_message);
    [[nodiscard]] absl::StatusOr<std::optional<std::size_t>> MaybeChooseFlushConversationItem() const;
    [[nodiscard]] absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>>
    MaybeCaptureFlushCandidate(const Message& assistant_message);
    [[nodiscard]] absl::Status
    QueueMidTermFlush(const OngoingEpisodeFlushCandidate& flush_candidate);
    [[nodiscard]] std::string NextEpisodeId();

    struct PendingMidTermFlush {
        // Owned and polled only by the orchestrator thread. The future's async task may run on a
        // worker thread, but pending_mid_term_flushes_ itself is not shared concurrently.
        std::size_t conversation_item_index = 0;
        std::future<absl::StatusOr<CompletedOngoingEpisodeFlush>> future;
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
