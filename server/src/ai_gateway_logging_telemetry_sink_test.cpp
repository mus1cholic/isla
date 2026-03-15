#include "isla/server/ai_gateway_logging_telemetry_sink.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

class CapturingLogSink final : public absl::LogSink {
  public:
    void Send(const absl::LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::string(entry.text_message()));
    }

    [[nodiscard]] bool Contains(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& message : messages_) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

TEST(AiGatewayLoggingTelemetrySinkTest, LogsPhaseTurnTotalAndFirstTokenMarkers) {
    CapturingLogSink capturing_sink;
    absl::AddLogSink(&capturing_sink);

    const std::shared_ptr<const TelemetrySink> telemetry_sink = CreateLoggingTelemetrySink();
    const std::shared_ptr<const TurnTelemetryContext> context =
        MakeTurnTelemetryContext("srv_test", "turn_1", telemetry_sink);
    const TurnTelemetryContext::Clock::time_point started_at = TurnTelemetryContext::Clock::now();
    const TurnTelemetryContext::Clock::time_point completed_at =
        started_at + std::chrono::milliseconds(12);

    RecordTelemetryPhase(context, telemetry::kPhaseExecutorTotal, started_at, completed_at);
    RecordTelemetryEvent(context, telemetry::kEventProviderFirstToken, completed_at);
    RecordTurnFinished(context, telemetry::kOutcomeSucceeded, completed_at);

    absl::RemoveLogSink(&capturing_sink);

    EXPECT_TRUE(capturing_sink.Contains("AI gateway telemetry phase"));
    EXPECT_TRUE(capturing_sink.Contains("phase=executor.total"));
    EXPECT_TRUE(capturing_sink.Contains("name=provider.first_token"));
    EXPECT_TRUE(capturing_sink.Contains("AI gateway telemetry turn.finished"));
    EXPECT_TRUE(capturing_sink.Contains("outcome=succeeded"));
}

} // namespace
} // namespace isla::server::ai_gateway
