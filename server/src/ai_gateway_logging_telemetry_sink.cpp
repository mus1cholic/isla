#include "isla/server/ai_gateway_logging_telemetry_sink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

std::optional<std::int64_t> DurationMillis(TurnTelemetryContext::Clock::time_point started_at,
                                           TurnTelemetryContext::Clock::time_point completed_at) {
    if (started_at == TurnTelemetryContext::Clock::time_point::min() || completed_at < started_at) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(completed_at - started_at).count();
}

class LoggingTelemetrySink final : public TelemetrySink {
  public:
    explicit LoggingTelemetrySink(LoggingTelemetrySinkConfig config) : config_(config) {}

    void OnTurnAccepted(const TurnTelemetryContext& context) const override {
        if (!config_.log_events) {
            return;
        }
        LOG(INFO) << "AI gateway telemetry turn.accepted session="
                  << SanitizeForLog(context.session_id)
                  << " turn_id=" << SanitizeForLog(context.turn_id);
    }

    void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at) const override {
        const std::optional<std::int64_t> offset_ms = DurationMillis(context.accepted_at, at);
        if (event_name == telemetry::kEventProviderFirstToken) {
            LOG(INFO) << "AI gateway telemetry event session=" << SanitizeForLog(context.session_id)
                      << " turn_id=" << SanitizeForLog(context.turn_id)
                      << " name=" << SanitizeForLog(event_name) << " offset_ms="
                      << (offset_ms.has_value() ? std::to_string(*offset_ms) : std::string("n/a"));
            return;
        }
        if (!config_.log_events) {
            return;
        }
        LOG(INFO) << "AI gateway telemetry event session=" << SanitizeForLog(context.session_id)
                  << " turn_id=" << SanitizeForLog(context.turn_id)
                  << " name=" << SanitizeForLog(event_name) << " offset_ms="
                  << (offset_ms.has_value() ? std::to_string(*offset_ms) : std::string("n/a"));
    }

    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        const std::optional<std::int64_t> duration_ms = DurationMillis(started_at, completed_at);
        const std::optional<std::int64_t> started_offset_ms =
            DurationMillis(context.accepted_at, started_at);
        const std::optional<std::int64_t> completed_offset_ms =
            DurationMillis(context.accepted_at, completed_at);
        LOG(INFO) << "AI gateway telemetry phase session=" << SanitizeForLog(context.session_id)
                  << " turn_id=" << SanitizeForLog(context.turn_id)
                  << " phase=" << SanitizeForLog(phase_name) << " duration_ms="
                  << (duration_ms.has_value() ? std::to_string(*duration_ms) : std::string("n/a"))
                  << " started_offset_ms="
                  << (started_offset_ms.has_value() ? std::to_string(*started_offset_ms)
                                                    : std::string("n/a"))
                  << " completed_offset_ms="
                  << (completed_offset_ms.has_value() ? std::to_string(*completed_offset_ms)
                                                      : std::string("n/a"));
    }

    void OnTurnFinished(const TurnTelemetryContext& context, std::string_view outcome,
                        TurnTelemetryContext::Clock::time_point finished_at) const override {
        const std::optional<std::int64_t> total_duration_ms =
            DurationMillis(context.accepted_at, finished_at);
        LOG(INFO) << "AI gateway telemetry turn.finished session="
                  << SanitizeForLog(context.session_id)
                  << " turn_id=" << SanitizeForLog(context.turn_id)
                  << " outcome=" << SanitizeForLog(outcome) << " total_duration_ms="
                  << (total_duration_ms.has_value() ? std::to_string(*total_duration_ms)
                                                    : std::string("n/a"));
    }

  private:
    LoggingTelemetrySinkConfig config_;
};

} // namespace

std::shared_ptr<const TelemetrySink> CreateLoggingTelemetrySink(LoggingTelemetrySinkConfig config) {
    return std::make_shared<LoggingTelemetrySink>(std::move(config));
}

} // namespace isla::server::ai_gateway
