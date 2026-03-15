#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace isla::server::ai_gateway {

class TelemetrySink;

namespace telemetry {

inline constexpr std::string_view kPhaseGatewayAccept = "gateway.accept";
inline constexpr std::string_view kPhaseMemoryUserQuery = "memory.user_query";
inline constexpr std::string_view kPhaseQueueWait = "queue.wait";
inline constexpr std::string_view kPhasePlanCreate = "plan.create";
inline constexpr std::string_view kPhaseExecutorTotal = "executor.total";
inline constexpr std::string_view kPhaseExecutorStep = "executor.step";
inline constexpr std::string_view kPhaseLlmProviderTotal = "llm.provider.total";
inline constexpr std::string_view kPhaseProviderSerializeRequest = "provider.serialize_request";
inline constexpr std::string_view kPhaseProviderTransport = "provider.transport";
inline constexpr std::string_view kPhaseProviderStream = "provider.stream";
inline constexpr std::string_view kPhaseProviderAggregateText = "provider.aggregate_text";
inline constexpr std::string_view kPhaseEmitTextOutput = "emit.text_output";
inline constexpr std::string_view kPhaseMemoryAssistantReply = "memory.assistant_reply";
inline constexpr std::string_view kPhaseEmitTurnCompleted = "emit.turn_completed";
inline constexpr std::string_view kPhaseEmitError = "emit.error";
inline constexpr std::string_view kPhaseEmitTurnCancelled = "emit.turn_cancelled";

inline constexpr std::string_view kEventTurnAccepted = "turn.accepted";
inline constexpr std::string_view kEventMemoryUserQueryStarted = "memory.user_query.started";
inline constexpr std::string_view kEventMemoryUserQueryCompleted = "memory.user_query.completed";
inline constexpr std::string_view kEventTurnEnqueued = "turn.enqueued";
inline constexpr std::string_view kEventTurnDequeued = "turn.dequeued";
inline constexpr std::string_view kEventPlanCreateStarted = "plan.create.started";
inline constexpr std::string_view kEventPlanCreateCompleted = "plan.create.completed";
inline constexpr std::string_view kEventExecutorStarted = "executor.started";
inline constexpr std::string_view kEventExecutorCompleted = "executor.completed";
inline constexpr std::string_view kEventProviderDispatched = "provider.dispatched";
inline constexpr std::string_view kEventProviderFirstToken = "provider.first_token";
inline constexpr std::string_view kEventProviderCompleted = "provider.completed";
inline constexpr std::string_view kEventTextOutputEmitStarted = "text_output.emit.started";
inline constexpr std::string_view kEventTextOutputEmitCompleted = "text_output.emit.completed";
inline constexpr std::string_view kEventErrorEmitStarted = "error.emit.started";
inline constexpr std::string_view kEventErrorEmitCompleted = "error.emit.completed";
inline constexpr std::string_view kEventMemoryAssistantReplyStarted =
    "memory.assistant_reply.started";
inline constexpr std::string_view kEventMemoryAssistantReplyCompleted =
    "memory.assistant_reply.completed";
inline constexpr std::string_view kEventTurnCancelledEmitStarted = "turn.cancelled.emit.started";
inline constexpr std::string_view kEventTurnCancelledEmitCompleted =
    "turn.cancelled.emit.completed";
inline constexpr std::string_view kEventTurnCompletedEmitStarted = "turn.completed.emit.started";
inline constexpr std::string_view kEventTurnCompletedEmitCompleted =
    "turn.completed.emit.completed";
inline constexpr std::string_view kEventTurnFailed = "turn.failed";

inline constexpr std::string_view kOutcomeSucceeded = "succeeded";
inline constexpr std::string_view kOutcomeFailed = "failed";
inline constexpr std::string_view kOutcomeCancelled = "cancelled";

} // namespace telemetry

struct TurnTelemetryContext {
    using Clock = std::chrono::steady_clock;

    std::shared_ptr<const TelemetrySink> sink;
    std::string session_id;
    std::string turn_id;
    Clock::time_point accepted_at = Clock::time_point::min();
};

class TelemetrySink {
  public:
    virtual ~TelemetrySink() = default;

    virtual void OnTurnAccepted(const TurnTelemetryContext& context) const {
        static_cast<void>(context);
    }

    virtual void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                         TurnTelemetryContext::Clock::time_point at) const {
        static_cast<void>(context);
        static_cast<void>(event_name);
        static_cast<void>(at);
    }

    virtual void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                         TurnTelemetryContext::Clock::time_point started_at,
                         TurnTelemetryContext::Clock::time_point completed_at) const {
        static_cast<void>(context);
        static_cast<void>(phase_name);
        static_cast<void>(started_at);
        static_cast<void>(completed_at);
    }

    virtual void OnTurnFinished(const TurnTelemetryContext& context, std::string_view outcome,
                                TurnTelemetryContext::Clock::time_point finished_at) const {
        static_cast<void>(context);
        static_cast<void>(outcome);
        static_cast<void>(finished_at);
    }
};

namespace internal {

class NoOpTelemetrySink final : public TelemetrySink {};

inline void SwallowTelemetryException() noexcept {
    // Telemetry must never terminate gateway control flow, especially from noexcept paths such as
    // destructors and move operations.
}

} // namespace internal

[[nodiscard]] inline std::shared_ptr<const TelemetrySink> CreateNoOpTelemetrySink() {
    static const std::shared_ptr<const TelemetrySink> sink =
        std::make_shared<internal::NoOpTelemetrySink>();
    return sink;
}

[[nodiscard]] inline std::shared_ptr<const TelemetrySink>
NormalizeTelemetrySink(std::shared_ptr<const TelemetrySink> sink) {
    return sink != nullptr ? std::move(sink) : CreateNoOpTelemetrySink();
}

inline void RecordTelemetryEvent(
    const std::shared_ptr<const TurnTelemetryContext>& context, std::string_view event_name,
    TurnTelemetryContext::Clock::time_point at = TurnTelemetryContext::Clock::now()) {
    if (context == nullptr || context->sink == nullptr) {
        return;
    }
    try {
        context->sink->OnEvent(*context, event_name, at);
    } catch (...) {
        internal::SwallowTelemetryException();
    }
}

inline void RecordTelemetryPhase(const std::shared_ptr<const TurnTelemetryContext>& context,
                                 std::string_view phase_name,
                                 TurnTelemetryContext::Clock::time_point started_at,
                                 TurnTelemetryContext::Clock::time_point completed_at) {
    if (context == nullptr || context->sink == nullptr) {
        return;
    }
    try {
        context->sink->OnPhase(*context, phase_name, started_at, completed_at);
    } catch (...) {
        internal::SwallowTelemetryException();
    }
}

inline void RecordTurnFinished(
    const std::shared_ptr<const TurnTelemetryContext>& context, std::string_view outcome,
    TurnTelemetryContext::Clock::time_point finished_at = TurnTelemetryContext::Clock::now()) {
    if (context == nullptr || context->sink == nullptr) {
        return;
    }
    try {
        context->sink->OnTurnFinished(*context, outcome, finished_at);
    } catch (...) {
        internal::SwallowTelemetryException();
    }
}

// ScopedTelemetryPhase emits the optional started event at construction time and records the phase
// exactly once on Finish() or destruction. If a caller finishes the phase early, later destruction
// is a no-op. When provided, the completed event is emitted at the same timestamp as the recorded
// phase completion.
class ScopedTelemetryPhase {
  public:
    ScopedTelemetryPhase(
        std::shared_ptr<const TurnTelemetryContext> context, std::string_view phase_name,
        std::string_view started_event = {}, std::string_view completed_event = {},
        TurnTelemetryContext::Clock::time_point started_at = TurnTelemetryContext::Clock::now())
        : context_(std::move(context)), phase_name_(phase_name), started_event_(started_event),
          completed_event_(completed_event), started_at_(started_at) {
        if (!started_event_.empty()) {
            RecordTelemetryEvent(context_, started_event_, started_at_);
        }
    }

    ScopedTelemetryPhase(const ScopedTelemetryPhase&) = delete;
    ScopedTelemetryPhase& operator=(const ScopedTelemetryPhase&) = delete;

    ScopedTelemetryPhase(ScopedTelemetryPhase&& other) noexcept
        : context_(std::move(other.context_)), phase_name_(other.phase_name_),
          started_event_(other.started_event_), completed_event_(other.completed_event_),
          started_at_(other.started_at_), active_(other.active_) {
        other.active_ = false;
    }

    ScopedTelemetryPhase& operator=(ScopedTelemetryPhase&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        try {
            Finish();
        } catch (...) {
            internal::SwallowTelemetryException();
        }
        context_ = std::move(other.context_);
        phase_name_ = other.phase_name_;
        started_event_ = other.started_event_;
        completed_event_ = other.completed_event_;
        started_at_ = other.started_at_;
        active_ = other.active_;
        other.active_ = false;
        return *this;
    }

    ~ScopedTelemetryPhase() noexcept {
        try {
            Finish();
        } catch (...) {
            internal::SwallowTelemetryException();
        }
    }

    void Finish(
        TurnTelemetryContext::Clock::time_point completed_at = TurnTelemetryContext::Clock::now()) {
        if (!active_) {
            return;
        }
        active_ = false;
        RecordTelemetryPhase(context_, phase_name_, started_at_, completed_at);
        if (!completed_event_.empty()) {
            RecordTelemetryEvent(context_, completed_event_, completed_at);
        }
    }

  private:
    std::shared_ptr<const TurnTelemetryContext> context_;
    std::string phase_name_;
    std::string started_event_;
    std::string completed_event_;
    TurnTelemetryContext::Clock::time_point started_at_;
    bool active_ = true;
};

[[nodiscard]] inline std::shared_ptr<const TurnTelemetryContext> MakeTurnTelemetryContext(
    std::string_view session_id, std::string_view turn_id,
    std::shared_ptr<const TelemetrySink> sink = nullptr,
    TurnTelemetryContext::Clock::time_point accepted_at = TurnTelemetryContext::Clock::now()) {
    auto context = std::make_shared<TurnTelemetryContext>();
    context->sink = NormalizeTelemetrySink(std::move(sink));
    context->session_id = std::string(session_id);
    context->turn_id = std::string(turn_id);
    context->accepted_at = accepted_at;
    context->sink->OnTurnAccepted(*context);
    return context;
}

} // namespace isla::server::ai_gateway
