#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace isla::server::ai_gateway {

class TelemetrySink;

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
};

namespace internal {

class NoOpTelemetrySink final : public TelemetrySink {};

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
