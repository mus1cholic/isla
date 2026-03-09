#include "isla/server/ai_gateway_stub_responder.hpp"

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "isla/server/ai_gateway_stub_responder_utils.hpp"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;

template <typename StartFn>
absl::Status await_emit(std::chrono::milliseconds timeout, StartFn&& start) {
    auto promise = std::make_shared<std::promise<absl::Status>>();
    std::future<absl::Status> future = promise->get_future();
    start([promise](absl::Status status) { promise->set_value(std::move(status)); });
    if (future.wait_for(timeout) != std::future_status::ready) {
        return absl::DeadlineExceededError("timed out waiting for async emit completion");
    }
    return future.get();
}

} // namespace

GatewayStubResponder::GatewayStubResponder(GatewayStubResponderConfig config)
    : config_(std::move(config)) {
    worker_ = std::thread([this] {
        try {
            WorkerLoop();
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway stub worker terminated with exception detail='"
                       << SanitizeForLog(error.what()) << "'";
        } catch (...) {
            LOG(ERROR) << "AI gateway stub worker terminated with unknown exception";
        }
    });
}

GatewayStubResponder::~GatewayStubResponder() {
    StopWorker();
}

void GatewayStubResponder::AttachSessionRegistry(GatewaySessionRegistry* session_registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_registry_ = session_registry;
}

void GatewayStubResponder::OnSessionStarted(const SessionStartedEvent& event) {
    LOG(INFO) << "AI gateway stub observed session start session=" << event.session_id;
    const absl::Status status = InitializeSessionMemory(event.session_id);
    if (!status.ok()) {
        LOG(ERROR) << "AI gateway stub failed to initialize session memory session="
                   << event.session_id << " detail='" << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::OnTurnAccepted(const TurnAcceptedEvent& event) {
    LOG(INFO) << "AI gateway stub accepted turn session=" << event.session_id
              << " turn_id=" << SanitizeForLog(event.turn_id);

    bool stopping = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++accepted_turns_count_;
        stopping = stopping_;
    }
    if (stopping) {
        AsyncFinishServerStoppingTurn(PendingTurn{
            .session_id = event.session_id,
            .turn_id = event.turn_id,
            .text = event.text,
            .ready_at = Clock::now(),
            .cancel_requested = false,
        });
        return;
    }

    const absl::StatusOr<isla::server::memory::UserQueryMemoryResult> memory_result =
        HandleAcceptedTurnMemory(event);
    if (!memory_result.ok()) {
        LOG(ERROR) << "AI gateway stub failed to process turn memory session=" << event.session_id
                   << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='"
                   << SanitizeForLog(memory_result.status().message()) << "'";
        BestEffortTerminateAcceptedTurn(
            PendingTurn{
                .session_id = event.session_id,
                .turn_id = event.turn_id,
                .text = event.text,
                .ready_at = Clock::now(),
                .cancel_requested = false,
            },
            "internal_error", "stub responder failed to update memory", "memory update failure");
        return;
    }
    VLOG(1) << "AI gateway stub prepared user prompt session=" << event.session_id
            << " turn_id=" << SanitizeForLog(event.turn_id)
            << " working_memory_bytes=" << memory_result->rendered_working_memory.size();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        VLOG(1) << "AI gateway stub queued turn session=" << event.session_id
                << " turn_id=" << SanitizeForLog(event.turn_id)
                << " delay_ms=" << config_.response_delay.count();
        pending_turns_.insert_or_assign(event.session_id,
                                        PendingTurn{
                                            .session_id = event.session_id,
                                            .turn_id = event.turn_id,
                                            .text = event.text,
                                            .ready_at = Clock::now() + config_.response_delay,
                                            .cancel_requested = false,
                                        });
    }

    cv_.notify_all();
}

void GatewayStubResponder::OnTurnCancelRequested(const TurnCancelRequestedEvent& event) {
    LOG(INFO) << "AI gateway stub received cancel session=" << event.session_id
              << " turn_id=" << SanitizeForLog(event.turn_id);

    if (!TryMarkTrackedTurnCancelled(event.session_id, event.turn_id)) {
        LOG(WARNING) << "AI gateway stub ignored cancel for untracked turn session="
                     << event.session_id << " turn_id=" << SanitizeForLog(event.turn_id);
        return;
    }
}

void GatewayStubResponder::OnSessionClosed(const SessionClosedEvent& event) {
    LOG(INFO) << "AI gateway stub observed session close session=" << event.session_id
              << " detail='" << SanitizeForLog(event.detail) << "'";

    std::lock_guard<std::mutex> lock(mutex_);
    const auto pending_erased = pending_turns_.erase(event.session_id);
    const auto in_progress_erased = in_progress_turns_.erase(event.session_id);
    memory_by_session_.erase(event.session_id);
    if (pending_erased > 0U || in_progress_erased > 0U) {
        VLOG(1) << "AI gateway stub dropped pending turn for closed session session="
                << event.session_id;
    }
}

void GatewayStubResponder::OnServerStopping(GatewaySessionRegistry& session_registry) {
    AttachSessionRegistry(&session_registry);

    std::vector<PendingTurn> turns_to_finalize;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        turns_to_finalize.reserve(pending_turns_.size() + in_progress_turns_.size());
        for (const auto& entry : pending_turns_) {
            turns_to_finalize.push_back(entry.second);
        }
        for (const auto& entry : in_progress_turns_) {
            turns_to_finalize.push_back(entry.second);
        }
        pending_turns_.clear();
        in_progress_turns_.clear();
        memory_by_session_.clear();
    }

    VLOG(1) << "AI gateway stub stopping pending_turns=" << turns_to_finalize.size();

    StopWorker();

    for (const PendingTurn& turn : turns_to_finalize) {
        FinishServerStoppingTurn(session_registry, turn);
    }
}

void GatewayStubResponder::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void GatewayStubResponder::WorkerLoop() {
    for (;;) {
        std::optional<PendingTurn> next_turn;
        std::optional<Clock::time_point> next_deadline;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (worker_stop_requested_) {
                    return;
                }

                const Clock::time_point now = Clock::now();
                for (auto it = pending_turns_.begin(); it != pending_turns_.end(); ++it) {
                    PendingTurn& turn = it->second;
                    if (turn.cancel_requested) {
                        next_turn = turn;
                        VLOG(1) << "AI gateway stub dequeued cancellation session=" << it->first
                                << " turn_id=" << SanitizeForLog(turn.turn_id);
                        in_progress_turns_.insert_or_assign(it->first, turn);
                        pending_turns_.erase(it);
                        break;
                    }
                    if (turn.ready_at <= now) {
                        next_turn = turn;
                        VLOG(1) << "AI gateway stub dequeued ready turn session=" << it->first
                                << " turn_id=" << SanitizeForLog(turn.turn_id);
                        in_progress_turns_.insert_or_assign(it->first, turn);
                        pending_turns_.erase(it);
                        break;
                    }
                    if (!next_deadline.has_value() || turn.ready_at < *next_deadline) {
                        next_deadline = turn.ready_at;
                    }
                }

                if (next_turn.has_value()) {
                    break;
                }

                if (next_deadline.has_value()) {
                    cv_.wait_until(lock, *next_deadline);
                } else {
                    cv_.wait(lock);
                }
                next_deadline.reset();
            }
        }

        if (!next_turn.has_value()) {
            continue;
        }

        // Phase 2.5 keeps a single blocking worker so stub turn completion stays deterministic and
        // transport orchestration remains simple before later executor/provider phases add richer
        // concurrency.
        try {
            if (next_turn->cancel_requested) {
                FinishCancelledTurn(*next_turn);
            } else {
                FinishSuccessfulTurn(*next_turn);
            }
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway stub turn processing threw session=" << next_turn->session_id
                       << " turn_id=" << SanitizeForLog(next_turn->turn_id) << " detail='"
                       << SanitizeForLog(error.what()) << "'";
            FinishProcessingExceptionTurn(*next_turn, error.what());
        } catch (...) {
            LOG(ERROR) << "AI gateway stub turn processing threw session=" << next_turn->session_id
                       << " turn_id=" << SanitizeForLog(next_turn->turn_id)
                       << " detail='unknown exception'";
            FinishProcessingExceptionTurn(*next_turn, "unknown exception");
        }
        ForgetInProgressTurn(next_turn->session_id, next_turn->turn_id);
    }
}

bool GatewayStubResponder::TryMarkTrackedTurnCancelled(std::string_view session_id,
                                                       std::string_view turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto pending = pending_turns_.find(std::string(session_id));
        pending != pending_turns_.end()) {
        if (pending->second.turn_id != turn_id) {
            return false;
        }
        pending->second.ready_at = Clock::now();
        pending->second.cancel_requested = true;
        VLOG(1) << "AI gateway stub queued cancellation session=" << session_id
                << " turn_id=" << SanitizeForLog(turn_id);
        cv_.notify_all();
        return true;
    }

    if (auto in_progress = in_progress_turns_.find(std::string(session_id));
        in_progress != in_progress_turns_.end()) {
        if (in_progress->second.turn_id != turn_id) {
            return false;
        }
        in_progress->second.cancel_requested = true;
        VLOG(1) << "AI gateway stub marked in-progress cancellation session=" << session_id
                << " turn_id=" << SanitizeForLog(turn_id);
        return true;
    }

    return false;
}

bool GatewayStubResponder::IsTrackedTurnCancelled(std::string_view session_id,
                                                  std::string_view turn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto in_progress = in_progress_turns_.find(std::string(session_id));
    return in_progress != in_progress_turns_.end() && in_progress->second.turn_id == turn_id &&
           in_progress->second.cancel_requested;
}

bool GatewayStubResponder::ShouldAbortTrackedTurn(std::string_view session_id,
                                                  std::string_view turn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
        return false;
    }
    const auto in_progress = in_progress_turns_.find(std::string(session_id));
    return in_progress == in_progress_turns_.end() || in_progress->second.turn_id != turn_id;
}

void GatewayStubResponder::ForgetInProgressTurn(std::string_view session_id,
                                                std::string_view turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = in_progress_turns_.find(std::string(session_id));
    if (it != in_progress_turns_.end() && it->second.turn_id == turn_id) {
        in_progress_turns_.erase(it);
    }
}

void GatewayStubResponder::AsyncFinishServerStoppingTurn(const PendingTurn& turn) {
    const std::shared_ptr<GatewayLiveSession> live_session =
        session_registry() == nullptr ? nullptr : session_registry()->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session during async shutdown session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.cancel_requested) {
        VLOG(1) << "AI gateway stub asynchronously emitting shutdown cancellation session="
                << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        live_session->AsyncEmitTurnCancelled(
            turn.turn_id,
            [session_id = turn.session_id, turn_id = turn.turn_id](absl::Status status) {
                if (!status.ok()) {
                    LOG(WARNING) << "AI gateway stub failed async shutdown cancellation session="
                                 << session_id << " turn_id=" << SanitizeForLog(turn_id)
                                 << " detail='" << SanitizeForLog(status.message()) << "'";
                }
            });
        return;
    }

    VLOG(1) << "AI gateway stub asynchronously emitting shutdown terminal error session="
            << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
    live_session->AsyncEmitError(
        turn.turn_id, "server_stopping", "server stopping",
        [live_session, session_id = turn.session_id, turn_id = turn.turn_id](absl::Status status) {
            if (!status.ok()) {
                LOG(WARNING) << "AI gateway stub failed async shutdown error session=" << session_id
                             << " turn_id=" << SanitizeForLog(turn_id) << " detail='"
                             << SanitizeForLog(status.message()) << "'";
                return;
            }
            live_session->AsyncEmitTurnCompleted(
                turn_id, [session_id, turn_id](absl::Status completion_status) {
                    if (!completion_status.ok()) {
                        LOG(WARNING)
                            << "AI gateway stub failed async shutdown completion session="
                            << session_id << " turn_id=" << SanitizeForLog(turn_id) << " detail='"
                            << SanitizeForLog(completion_status.message()) << "'";
                    }
                });
        });
}

void GatewayStubResponder::BestEffortTerminateAcceptedTurn(const PendingTurn& turn,
                                                           std::string_view code,
                                                           std::string_view message,
                                                           std::string_view log_context) noexcept {
    try {
        GatewaySessionRegistry* registry = session_registry();
        if (registry == nullptr) {
            LOG(WARNING) << "AI gateway stub missing session registry during " << log_context
                         << " session=" << turn.session_id
                         << " turn_id=" << SanitizeForLog(turn.turn_id);
            return;
        }

        const std::shared_ptr<GatewayLiveSession> live_session =
            registry->FindSession(turn.session_id);
        if (live_session == nullptr) {
            LOG(WARNING) << "AI gateway stub lost live session during " << log_context
                         << " session=" << turn.session_id
                         << " turn_id=" << SanitizeForLog(turn.turn_id);
            return;
        }

        if (!code.empty() && !message.empty()) {
            const absl::Status error_status =
                await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                    live_session->AsyncEmitError(turn.turn_id, std::string(code),
                                                 std::string(message), std::move(on_complete));
                });
            if (!error_status.ok()) {
                LOG(WARNING) << "AI gateway stub failed to emit follow-up error during "
                             << log_context << " session=" << turn.session_id
                             << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                             << SanitizeForLog(error_status.message()) << "'";
            }
        }

        const absl::Status completion_status =
            await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
            });
        if (!completion_status.ok()) {
            LOG(WARNING) << "AI gateway stub failed to emit follow-up completion during "
                         << log_context << " session=" << turn.session_id
                         << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                         << SanitizeForLog(completion_status.message()) << "'";
        }
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway stub follow-up terminalization failed during " << log_context
                   << " session=" << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " detail='" << SanitizeForLog(error.what()) << "'";
    } catch (...) {
        LOG(ERROR) << "AI gateway stub follow-up terminalization failed during " << log_context
                   << " session=" << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " detail='unknown exception'";
    }
}

void GatewayStubResponder::FinishProcessingExceptionTurn(const PendingTurn& turn,
                                                         std::string_view detail) noexcept {
    BestEffortTerminateAcceptedTurn(turn, "internal_error", "stub responder processing failed",
                                    "processing failure");
    VLOG(1) << "AI gateway stub terminalized failed turn session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='" << SanitizeForLog(detail)
            << "'";
}

void GatewayStubResponder::FinishSuccessfulTurn(const PendingTurn& turn) {
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted successful turn during shutdown session="
                << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        return;
    }

    GatewaySessionRegistry* registry = session_registry();
    if (registry == nullptr) {
        LOG(WARNING) << "AI gateway stub missing session registry for session=" << turn.session_id;
        return;
    }

    const std::shared_ptr<GatewayLiveSession> live_session = registry->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session before reply session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.text.size() > kMaxTextInputBytes) {
        LOG(ERROR) << "AI gateway stub rejected oversized accepted turn session=" << turn.session_id
                   << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " text_bytes=" << turn.text.size();
        BestEffortTerminateAcceptedTurn(turn, "bad_request",
                                        "text.input text exceeds maximum length", "oversized turn");
        return;
    }

    const std::string reply = config_.reply_builder
                                  ? config_.reply_builder(config_.response_prefix, turn.text)
                                  : BuildStubReply(config_.response_prefix, turn.text);
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted reply emission during shutdown session="
                << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        return;
    }
    VLOG(1) << "AI gateway stub emitting successful reply session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            live_session->AsyncEmitTextOutput(turn.turn_id, reply, std::move(on_complete));
        });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit text output session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder failed to emit text output",
                                        "text output failure");
        return;
    }

    status = HandleSuccessfulReplyMemory(turn, reply);
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to record assistant reply in memory session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder failed to update memory",
                                        "memory update failure");
        return;
    }

    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted completion during shutdown session=" << turn.session_id
                << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        return;
    }

    status = await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn completion session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::FinishCancelledTurn(const PendingTurn& turn) {
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted cancellation during shutdown session="
                << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    GatewaySessionRegistry* registry = session_registry();
    if (registry == nullptr) {
        LOG(WARNING) << "AI gateway stub missing session registry for cancel session="
                     << turn.session_id;
        return;
    }

    const std::shared_ptr<GatewayLiveSession> live_session = registry->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session before cancel session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    VLOG(1) << "AI gateway stub emitting cancellation session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    const absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            live_session->AsyncEmitTurnCancelled(turn.turn_id, std::move(on_complete));
        });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn cancelled session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                                    const PendingTurn& turn) {
    const std::shared_ptr<GatewayLiveSession> live_session =
        session_registry.FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session during shutdown session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.cancel_requested) {
        VLOG(1) << "AI gateway stub emitting shutdown cancellation session=" << turn.session_id
                << " turn_id=" << SanitizeForLog(turn.turn_id);
        const absl::Status status =
            await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                live_session->AsyncEmitTurnCancelled(turn.turn_id, std::move(on_complete));
            });
        if (!status.ok()) {
            LOG(WARNING) << "AI gateway stub failed to emit shutdown cancellation session="
                         << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                         << " detail='" << SanitizeForLog(status.message()) << "'";
        }
        return;
    }

    VLOG(1) << "AI gateway stub emitting shutdown terminal error session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            live_session->AsyncEmitError(turn.turn_id, "server_stopping", "server stopping",
                                         std::move(on_complete));
        });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown error session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return;
    }

    status = await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown completion session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
    }
}

GatewaySessionRegistry* GatewayStubResponder::session_registry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_registry_;
}

std::shared_ptr<GatewayStubResponder::SessionMemoryState>
GatewayStubResponder::FindSessionMemory(std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = memory_by_session_.find(std::string(session_id));
    if (it == memory_by_session_.end()) {
        return nullptr;
    }
    return it->second;
}

absl::Status GatewayStubResponder::InitializeSessionMemory(std::string_view session_id) {
    const bool using_bundled_system_prompt = config_.memory_system_prompt.empty();
    const std::string system_prompt = using_bundled_system_prompt
                                          ? isla::server::memory::DefaultSystemPrompt()
                                          : config_.memory_system_prompt;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto [it, inserted] = memory_by_session_.try_emplace(
        std::string(session_id),
        std::make_shared<SessionMemoryState>(isla::server::memory::MemoryOrchestrator::Create(
            std::string(session_id), isla::server::memory::WorkingMemoryInit{
                                         .system_prompt = system_prompt,
                                         .user_id = config_.memory_user_id,
                                     })));
    static_cast<void>(it);
    if (!inserted) {
        return absl::AlreadyExistsError("memory orchestrator already exists for session");
    }
    VLOG(1) << "AI gateway stub initialized session memory session=" << session_id
            << " system_prompt_source="
            << (using_bundled_system_prompt ? "bundled_default" : "explicit_override")
            << " system_prompt_bytes=" << system_prompt.size();
    return absl::OkStatus();
}

absl::StatusOr<isla::server::memory::UserQueryMemoryResult>
GatewayStubResponder::HandleAcceptedTurnMemory(const TurnAcceptedEvent& event) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(event.session_id);
    if (session_memory == nullptr) {
        return absl::FailedPreconditionError("missing memory orchestrator for started session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    absl::StatusOr<isla::server::memory::UserQueryMemoryResult> result =
        session_memory->orchestrator.HandleUserQuery(isla::server::memory::GatewayUserQuery(
            event.session_id, event.turn_id, event.text, NowTimestamp()));
    if (result.ok() && config_.on_user_query_memory_ready) {
        config_.on_user_query_memory_ready(event.session_id, *result);
    }
    return result;
}

absl::Status GatewayStubResponder::HandleSuccessfulReplyMemory(const PendingTurn& turn,
                                                               std::string_view reply_text) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(turn.session_id);
    if (session_memory == nullptr) {
        return absl::FailedPreconditionError("missing memory orchestrator for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.HandleAssistantReply(
        isla::server::memory::GatewayAssistantReply(turn.session_id, turn.turn_id,
                                                    std::string(reply_text), NowTimestamp()));
}

absl::StatusOr<std::string>
GatewayStubResponder::RenderSessionMemoryPrompt(std::string_view session_id) const {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory == nullptr) {
        return absl::NotFoundError("memory orchestrator not found for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.RenderFullWorkingMemory();
}

bool GatewayStubResponder::WaitForAcceptedTurns(std::size_t expected_count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2),
                        [this, expected_count] { return accepted_turns_count_ >= expected_count; });
}

} // namespace isla::server::ai_gateway
