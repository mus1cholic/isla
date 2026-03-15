#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server::ai_gateway::test {

struct TelemetryEventRecord {
    std::string name;
    TurnTelemetryContext::Clock::time_point at;
};

struct TelemetryPhaseRecord {
    std::string name;
    TurnTelemetryContext::Clock::time_point started_at;
    TurnTelemetryContext::Clock::time_point completed_at;
};

struct TurnFinishedRecord {
    std::string outcome;
    TurnTelemetryContext::Clock::time_point finished_at;
};

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at) const override {
        static_cast<void>(context);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(TelemetryEventRecord{
                .name = std::string(event_name),
                .at = at,
            });
        }
        cv_.notify_all();
    }

    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        static_cast<void>(context);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phases_.push_back(TelemetryPhaseRecord{
                .name = std::string(phase_name),
                .started_at = started_at,
                .completed_at = completed_at,
            });
        }
        cv_.notify_all();
    }

    void OnTurnFinished(const TurnTelemetryContext& context, std::string_view outcome,
                        TurnTelemetryContext::Clock::time_point finished_at) const override {
        static_cast<void>(context);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_.push_back(TurnFinishedRecord{
                .outcome = std::string(outcome),
                .finished_at = finished_at,
            });
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<TelemetryEventRecord> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    [[nodiscard]] std::vector<TelemetryPhaseRecord> phases() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phases_;
    }

    [[nodiscard]] std::vector<TurnFinishedRecord> finished() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

    [[nodiscard]] bool WaitForFinishedCount(
        std::size_t expected_count,
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return finished_.size() >= expected_count; });
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable std::vector<TelemetryEventRecord> events_;
    mutable std::vector<TelemetryPhaseRecord> phases_;
    mutable std::vector<TurnFinishedRecord> finished_;
};

inline bool ContainsTelemetryEvent(const std::vector<TelemetryEventRecord>& events,
                                   std::string_view name) {
    for (const auto& event : events) {
        if (event.name == name) {
            return true;
        }
    }
    return false;
}

inline bool ContainsTelemetryPhase(const std::vector<TelemetryPhaseRecord>& phases,
                                   std::string_view name) {
    for (const auto& phase : phases) {
        if (phase.name == name) {
            return true;
        }
    }
    return false;
}

} // namespace isla::server::ai_gateway::test
