#include "ai_gateway_stub_responder.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;

template <typename StartFn> absl::Status await_emit(StartFn&& start) {
    auto promise = std::make_shared<std::promise<absl::Status>>();
    std::future<absl::Status> future = promise->get_future();
    start([promise](absl::Status status) { promise->set_value(std::move(status)); });
    if (future.wait_for(2s) != std::future_status::ready) {
        return absl::DeadlineExceededError("timed out waiting for async emit completion");
    }
    return future.get();
}

std::string build_stub_reply(std::string_view prefix, std::string_view text) {
    std::string reply;
    reply.reserve(prefix.size() + text.size());
    reply.append(prefix);
    reply.append(text);
    return reply;
}

} // namespace

GatewayStubResponder::GatewayStubResponder(GatewayStubResponderConfig config)
    : config_(std::move(config)), worker_([this] { WorkerLoop(); }) {}

GatewayStubResponder::~GatewayStubResponder() {
    StopWorker();
}

void GatewayStubResponder::AttachSessionRegistry(GatewaySessionRegistry* session_registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_registry_ = session_registry;
}

void GatewayStubResponder::OnTurnAccepted(const TurnAcceptedEvent& event) {
    LOG(INFO) << "AI gateway stub accepted turn session=" << event.session_id
              << " turn_id=" << SanitizeForLog(event.turn_id);

    bool stopping = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping = stopping_;
        if (!stopping) {
            VLOG(1) << "AI gateway stub queued turn session=" << event.session_id
                    << " turn_id=" << SanitizeForLog(event.turn_id)
                    << " delay_ms=" << config_.response_delay.count();
            pending_turns_[event.session_id] = PendingTurn{
                .session_id = event.session_id,
                .turn_id = event.turn_id,
                .text = event.text,
                .ready_at = Clock::now() + config_.response_delay,
                .cancel_requested = false,
                .finalizing = false,
            };
        }
    }

    if (stopping) {
        GatewaySessionRegistry* registry = session_registry();
        if (registry != nullptr) {
            FinishServerStoppingTurn(
                *registry,
                PendingTurn{
                    .session_id = event.session_id,
                    .turn_id = event.turn_id,
                    .text = event.text,
                    .ready_at = Clock::now(),
                    .cancel_requested = false,
                    .finalizing = true,
                });
        }
        return;
    }

    cv_.notify_all();
}

void GatewayStubResponder::OnTurnCancelRequested(const TurnCancelRequestedEvent& event) {
    LOG(INFO) << "AI gateway stub received cancel session=" << event.session_id
              << " turn_id=" << SanitizeForLog(event.turn_id);

    bool stopping = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping = stopping_;
        auto& pending = pending_turns_[event.session_id];
        pending.session_id = event.session_id;
        pending.turn_id = event.turn_id;
        pending.ready_at = Clock::now();
        pending.cancel_requested = true;
        VLOG(1) << "AI gateway stub queued cancellation session=" << event.session_id
                << " turn_id=" << SanitizeForLog(event.turn_id);
    }

    if (stopping) {
        GatewaySessionRegistry* registry = session_registry();
        if (registry == nullptr) {
            return;
        }
        FinishCancelledTurn(PendingTurn{
            .session_id = event.session_id,
            .turn_id = event.turn_id,
            .ready_at = Clock::now(),
            .cancel_requested = true,
            .finalizing = true,
        });
        return;
    }

    cv_.notify_all();
}

void GatewayStubResponder::OnSessionClosed(const SessionClosedEvent& event) {
    LOG(INFO) << "AI gateway stub observed session close session=" << event.session_id
              << " detail='" << SanitizeForLog(event.detail) << "'";

    std::lock_guard<std::mutex> lock(mutex_);
    const auto erased = pending_turns_.erase(event.session_id);
    if (erased > 0U) {
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
        for (const auto& [session_id, turn] : pending_turns_) {
            if (!turn.finalizing) {
                turns_to_finalize.push_back(turn);
            }
        }
        pending_turns_.clear();
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
                for (auto& [session_id, turn] : pending_turns_) {
                    static_cast<void>(session_id);
                    if (turn.finalizing) {
                        continue;
                    }
                    if (turn.cancel_requested) {
                        turn.finalizing = true;
                        next_turn = turn;
                        break;
                    }
                    if (turn.ready_at <= now) {
                        turn.finalizing = true;
                        next_turn = turn;
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

        if (next_turn->cancel_requested) {
            FinishCancelledTurn(*next_turn);
        } else {
            FinishSuccessfulTurn(*next_turn);
        }
        ForgetTurn(next_turn->session_id, next_turn->turn_id);
    }
}

void GatewayStubResponder::FinishSuccessfulTurn(const PendingTurn& turn) {
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

    const std::string reply = build_stub_reply(config_.response_prefix, turn.text);
    VLOG(1) << "AI gateway stub emitting successful reply session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    absl::Status status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTextOutput(turn.turn_id, reply, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit text output session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return;
    }

    status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn completion session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::FinishCancelledTurn(const PendingTurn& turn) {
    GatewaySessionRegistry* registry = session_registry();
    if (registry == nullptr) {
        LOG(WARNING) << "AI gateway stub missing session registry for cancel session="
                     << turn.session_id;
        return;
    }

    const std::shared_ptr<GatewayLiveSession> live_session = registry->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session before cancel session=" << turn.session_id
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    VLOG(1) << "AI gateway stub emitting cancellation session=" << turn.session_id
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    const absl::Status status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCancelled(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn cancelled session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                                    const PendingTurn& turn) {
    const std::shared_ptr<GatewayLiveSession> live_session = session_registry.FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session during shutdown session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.cancel_requested) {
        VLOG(1) << "AI gateway stub emitting shutdown cancellation session=" << turn.session_id
                << " turn_id=" << SanitizeForLog(turn.turn_id);
        const absl::Status status = await_emit([&](GatewayEmitCallback on_complete) {
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
    absl::Status status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitError(turn.turn_id, "server_stopping", "server stopping",
                                     std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown error session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
        return;
    }

    status = await_emit([&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown completion session="
                     << turn.session_id << " turn_id=" << SanitizeForLog(turn.turn_id)
                     << " detail='" << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::ForgetTurn(std::string_view session_id, std::string_view turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_turns_.find(std::string(session_id));
    if (it == pending_turns_.end()) {
        return;
    }
    if (it->second.turn_id == turn_id) {
        VLOG(1) << "AI gateway stub forgetting pending turn session=" << session_id
                << " turn_id=" << SanitizeForLog(turn_id);
        pending_turns_.erase(it);
    }
}

GatewaySessionRegistry* GatewayStubResponder::session_registry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_registry_;
}

} // namespace isla::server::ai_gateway
