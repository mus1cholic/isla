#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "ai_gateway_server.hpp"
#include "isla/server/memory/memory_orchestrator.hpp"

namespace isla::server::ai_gateway {

struct GatewayStubResponderConfig {
    std::chrono::milliseconds response_delay{ 50 };
    std::chrono::milliseconds async_emit_timeout{ std::chrono::seconds(2) };
    std::string response_prefix = "stub echo: ";
    std::string memory_system_prompt = "You are Isla.";
    std::string memory_user_id = "gateway_user";
    std::function<std::string(std::string_view, std::string_view)> reply_builder;
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
    [[nodiscard]] absl::StatusOr<std::string>
    RenderSessionMemoryPrompt(std::string_view session_id) const;

  private:
    using Clock = std::chrono::steady_clock;

    struct PendingTurn {
        std::string session_id;
        std::string turn_id;
        std::string text;
        Clock::time_point ready_at = Clock::time_point::min();
        bool cancel_requested = false;
    };

    void StopWorker();
    void WorkerLoop();
    [[nodiscard]] bool TryMarkTrackedTurnCancelled(std::string_view session_id,
                                                   std::string_view turn_id);
    [[nodiscard]] bool IsTrackedTurnCancelled(std::string_view session_id,
                                              std::string_view turn_id) const;
    [[nodiscard]] bool ShouldAbortTrackedTurn(std::string_view session_id,
                                              std::string_view turn_id) const;
    void ForgetInProgressTurn(std::string_view session_id, std::string_view turn_id);
    void AsyncFinishServerStoppingTurn(const PendingTurn& turn);
    void BestEffortTerminateAcceptedTurn(const PendingTurn& turn, std::string_view code,
                                         std::string_view message,
                                         std::string_view log_context) noexcept;
    void FinishProcessingExceptionTurn(const PendingTurn& turn, std::string_view detail) noexcept;
    void FinishSuccessfulTurn(const PendingTurn& turn);
    void FinishCancelledTurn(const PendingTurn& turn);
    void FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                  const PendingTurn& turn);
    [[nodiscard]] GatewaySessionRegistry* session_registry() const;
    [[nodiscard]] absl::Status InitializeSessionMemory(std::string_view session_id);
    [[nodiscard]] absl::Status HandleAcceptedTurnMemory(const TurnAcceptedEvent& event);
    [[nodiscard]] absl::Status HandleSuccessfulReplyMemory(const PendingTurn& turn,
                                                           std::string_view reply_text);

    GatewayStubResponderConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    GatewaySessionRegistry* session_registry_ = nullptr;
    absl::flat_hash_map<std::string, PendingTurn> pending_turns_;
    absl::flat_hash_map<std::string, PendingTurn> in_progress_turns_;
    absl::flat_hash_map<std::string, isla::server::memory::MemoryOrchestrator> memory_by_session_;
    bool stopping_ = false;
    bool worker_stop_requested_ = false;
};

} // namespace isla::server::ai_gateway
