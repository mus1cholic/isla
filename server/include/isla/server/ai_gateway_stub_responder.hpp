#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_executor.hpp"
#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/embedding_client.hpp"
#include "isla/server/gemini_api_embedding_client.hpp"
#include "isla/server/memory/memory_orchestrator.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

inline constexpr std::string_view kDefaultMidTermMemoryModel = kDefaultMidTermFlushDeciderModel;

// Resolves session-scoped timestamps used by the responder's memory-facing replay path.
//
// Production calls may leave all methods unresolved and fall back to wall-clock time. Eval replay
// can provide deterministic chronology through one shared session-clock object instead of patching
// individual timestamp call sites.
class GatewaySessionClock {
  public:
    virtual ~GatewaySessionClock() = default;

    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveSessionStartTime(std::string_view session_id) const {
        static_cast<void>(session_id);
        return std::nullopt;
    }

    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveConversationMessageTime(std::string_view session_id, std::string_view turn_id,
                                   isla::server::memory::MessageRole role) const {
        static_cast<void>(session_id);
        static_cast<void>(turn_id);
        static_cast<void>(role);
        return std::nullopt;
    }

    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveEvaluationReferenceTime(std::string_view session_id) const {
        static_cast<void>(session_id);
        return std::nullopt;
    }
};

struct GatewayStubResponderConfig {
    std::chrono::milliseconds response_delay{ 50 };
    std::chrono::milliseconds async_emit_timeout{ std::chrono::seconds(2) };
    // Total number of attempts to persist a session on startup. A value of 0 is
    // treated as 1. At least one attempt is always made if a memory store is configured.
    std::size_t session_start_persistence_max_attempts = 3;
    std::chrono::milliseconds session_start_persistence_retry_delay{ 100 };
    std::string memory_user_id = "gateway_user";
    isla::server::memory::MemoryStorePtr memory_store;
    GatewayLlmRuntimeConfig llm_runtime_config;
    OpenAiResponsesClientConfig openai_config;
    std::shared_ptr<const OpenAiResponsesClient> openai_client;
    GeminiApiEmbeddingClientConfig gemini_api_embedding_config;
    std::shared_ptr<const isla::server::EmbeddingClient> embedding_client;
    std::function<void(const ExecutionPlan&)> on_execution_plan;
    std::shared_ptr<const GatewaySessionClock> session_clock;
    std::function<void(std::string_view, const isla::server::memory::UserQueryMemoryResult&)>
        on_user_query_memory_ready;
};

enum class GatewayAcceptedTurnTerminalState {
    kSucceeded,
    kFailed,
    kCancelled,
};

struct GatewayAcceptedTurnFailure {
    std::string code;
    std::string message;
};

struct GatewayAcceptedTurnResult {
    GatewayAcceptedTurnTerminalState state = GatewayAcceptedTurnTerminalState::kFailed;
    std::optional<std::string> reply_text;
    std::optional<GatewayAcceptedTurnFailure> failure;
};

class GatewayStubResponder final : public GatewayApplicationEventSink {
  public:
    explicit GatewayStubResponder(GatewayStubResponderConfig config = {});
    ~GatewayStubResponder() override;

    GatewayStubResponder(const GatewayStubResponder&) = delete;
    GatewayStubResponder& operator=(const GatewayStubResponder&) = delete;

    void AttachSessionRegistry(GatewaySessionRegistry* session_registry);

    void OnSessionStarted(const SessionStartedEvent& event) override;
    void OnTurnAccepted(const TurnAcceptedEvent& event) override;
    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override;
    void OnSessionClosed(const SessionClosedEvent& event) override;
    void OnServerStopping(GatewaySessionRegistry& session_registry) override;
    [[nodiscard]] absl::Status AppendSessionUserMessage(std::string_view session_id,
                                                        std::string_view turn_id,
                                                        std::string_view text);
    [[nodiscard]] absl::Status AppendSessionAssistantMessage(std::string_view session_id,
                                                             std::string_view turn_id,
                                                             std::string_view text);
    [[nodiscard]] absl::StatusOr<std::string>
    RenderSessionMemoryPrompt(std::string_view session_id) const;
    [[nodiscard]] absl::StatusOr<isla::server::memory::WorkingMemoryState>
    SnapshotSessionWorkingMemoryState(
        std::string_view session_id,
        std::chrono::milliseconds settle_timeout = std::chrono::milliseconds(0)) const;
    [[nodiscard]] absl::StatusOr<GatewayAcceptedTurnResult>
    RunAcceptedTurnToCompletion(const TurnAcceptedEvent& event);
    [[nodiscard]] bool WaitForAcceptedTurns(std::size_t expected_count);
    [[nodiscard]] bool IsMidTermMemoryConfigured() const;
    [[nodiscard]] bool IsMidTermMemoryAvailable() const;
    [[nodiscard]] const absl::Status& MidTermMemoryInitializationStatus() const;

  private:
    using Clock = std::chrono::steady_clock;

    struct SessionMemoryState {
        explicit SessionMemoryState(isla::server::memory::MemoryOrchestrator orchestrator_in)
            : orchestrator(std::move(orchestrator_in)) {}

        mutable std::mutex mutex;
        isla::server::memory::MemoryOrchestrator orchestrator;
    };

    struct PendingTurn {
        std::string session_id;
        std::string turn_id;
        std::string text;
        std::string rendered_system_prompt;
        std::string rendered_working_memory_context;
        std::shared_ptr<const TurnTelemetryContext> telemetry_context;
        Clock::time_point enqueued_at = Clock::time_point::min();
        Clock::time_point ready_at = Clock::time_point::min();
        bool cancel_requested = false;
    };

    void StopWorker();
    void WorkerLoop();
    void RecordDequeueTelemetry(const PendingTurn& turn, Clock::time_point dequeued_at) const;
    [[nodiscard]] bool TryMarkTrackedTurnCancelled(std::string_view session_id,
                                                   std::string_view turn_id);
    [[nodiscard]] bool IsTrackedTurnCancelled(std::string_view session_id,
                                              std::string_view turn_id) const;
    [[nodiscard]] bool ShouldAbortTrackedTurn(std::string_view session_id,
                                              std::string_view turn_id) const;
    void ForgetInProgressTurn(std::string_view session_id, std::string_view turn_id);
    [[nodiscard]] PendingTurn BuildPendingTurnShell(const TurnAcceptedEvent& event) const;
    [[nodiscard]] absl::StatusOr<PendingTurn> PrepareAcceptedTurn(const TurnAcceptedEvent& event);
    [[nodiscard]] std::optional<GatewayAcceptedTurnResult>
    ExecutePreparedTurnToTerminal(const PendingTurn& turn);
    void AsyncFinishServerStoppingTurn(const PendingTurn& turn);
    void BestEffortTerminateAcceptedTurn(const PendingTurn& turn, std::string_view code,
                                         std::string_view message,
                                         std::string_view log_context) noexcept;
    void FinishProcessingExceptionTurn(const PendingTurn& turn, std::string_view detail) noexcept;
    void FinishSuccessfulTurn(const PendingTurn& turn);
    void FinishCancelledTurn(const PendingTurn& turn);
    void BestEffortEmitSessionStartFailure(std::string_view session_id,
                                           const absl::Status& status) noexcept;
    void FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                  const PendingTurn& turn);
    [[nodiscard]] GatewaySessionRegistry* session_registry() const;
    [[nodiscard]] std::shared_ptr<SessionMemoryState>
    FindSessionMemory(std::string_view session_id) const;
    [[nodiscard]] std::optional<absl::Status>
    FindSessionStartFailure(std::string_view session_id) const;
    [[nodiscard]] absl::Status InitializeSessionMemory(std::string_view session_id);
    [[nodiscard]] isla::server::memory::Timestamp
    ResolveSessionStartTime(std::string_view session_id) const;
    [[nodiscard]] isla::server::memory::Timestamp
    ResolveConversationMessageTime(std::string_view session_id, std::string_view turn_id,
                                   isla::server::memory::MessageRole role) const;
    [[nodiscard]] absl::StatusOr<isla::server::memory::UserQueryMemoryResult>
    HandleAcceptedTurnMemory(const TurnAcceptedEvent& event);
    [[nodiscard]] absl::Status HandleSuccessfulReplyMemory(const PendingTurn& turn,
                                                           std::string_view reply_text);
    [[nodiscard]] absl::StatusOr<std::string>
    ExpandMidTermEpisodeForSession(std::string_view session_id, std::string_view episode_id) const;
    [[nodiscard]] std::optional<isla::server::tools::ToolExecutionContext>
    BuildToolExecutionContext(std::string_view session_id,
                              std::shared_ptr<const TurnTelemetryContext> telemetry_context) const;

    GatewayStubResponderConfig config_;
    GatewayPlanExecutor executor_;
    isla::server::memory::MidTermFlushDeciderPtr mid_term_flush_decider_;
    isla::server::memory::MidTermCompactorPtr mid_term_compactor_;
    bool mid_term_memory_configured_ = false;
    absl::Status mid_term_memory_initialization_status_ = absl::OkStatus();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    GatewaySessionRegistry* session_registry_ = nullptr;
    absl::flat_hash_map<std::string, PendingTurn> pending_turns_;
    absl::flat_hash_map<std::string, PendingTurn> in_progress_turns_;
    absl::flat_hash_map<std::string, std::shared_ptr<SessionMemoryState>> memory_by_session_;
    absl::flat_hash_map<std::string, absl::Status> failed_session_starts_;
    bool stopping_ = false;
    bool worker_stop_requested_ = false;
    std::size_t accepted_turns_count_ = 0;
};

} // namespace isla::server::ai_gateway
