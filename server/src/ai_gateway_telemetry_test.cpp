#include "isla/server/ai_gateway_telemetry.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at) const override {
        static_cast<void>(context);
        static_cast<void>(at);
        events.push_back(std::string(event_name));
    }

    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        static_cast<void>(context);
        phases.push_back(PhaseRecord{
            .name = std::string(phase_name),
            .started_at = started_at,
            .completed_at = completed_at,
        });
    }

    void OnTurnFinished(const TurnTelemetryContext& context, std::string_view outcome,
                        TurnTelemetryContext::Clock::time_point finished_at) const override {
        static_cast<void>(context);
        static_cast<void>(finished_at);
        outcomes.push_back(std::string(outcome));
    }

    struct PhaseRecord {
        std::string name;
        TurnTelemetryContext::Clock::time_point started_at;
        TurnTelemetryContext::Clock::time_point completed_at;
    };

    mutable std::vector<std::string> events;
    mutable std::vector<PhaseRecord> phases;
    mutable std::vector<std::string> outcomes;
};

TEST(AiGatewayTelemetryTest, ScopedPhaseExplicitFinishDoesNotDoubleRecordOnDestruction) {
    auto sink = std::make_shared<RecordingTelemetrySink>();
    const std::shared_ptr<const TurnTelemetryContext> context =
        MakeTurnTelemetryContext("srv_test", "turn_1", sink);

    {
        ScopedTelemetryPhase phase(context, telemetry::kPhaseExecutorTotal,
                                   telemetry::kEventExecutorStarted,
                                   telemetry::kEventExecutorCompleted);
        phase.Finish();
    }

    ASSERT_EQ(sink->events.size(), 2U);
    EXPECT_EQ(sink->events[0], telemetry::kEventExecutorStarted);
    EXPECT_EQ(sink->events[1], telemetry::kEventExecutorCompleted);
    ASSERT_EQ(sink->phases.size(), 1U);
    EXPECT_EQ(sink->phases[0].name, telemetry::kPhaseExecutorTotal);
}

TEST(AiGatewayTelemetryTest, ScopedPhaseMoveOnlyRecordsOnce) {
    auto sink = std::make_shared<RecordingTelemetrySink>();
    const std::shared_ptr<const TurnTelemetryContext> context =
        MakeTurnTelemetryContext("srv_test", "turn_1", sink);

    {
        ScopedTelemetryPhase original(context, telemetry::kPhasePlanCreate,
                                      telemetry::kEventPlanCreateStarted,
                                      telemetry::kEventPlanCreateCompleted);
        ScopedTelemetryPhase moved(std::move(original));
    }

    ASSERT_EQ(sink->events.size(), 2U);
    EXPECT_EQ(sink->events[0], telemetry::kEventPlanCreateStarted);
    EXPECT_EQ(sink->events[1], telemetry::kEventPlanCreateCompleted);
    ASSERT_EQ(sink->phases.size(), 1U);
    EXPECT_EQ(sink->phases[0].name, telemetry::kPhasePlanCreate);
}

TEST(AiGatewayTelemetryTest, RecordingHelpersNoOpForNullContext) {
    const std::shared_ptr<const TurnTelemetryContext> context;

    RecordTelemetryEvent(context, telemetry::kEventTurnAccepted);
    RecordTelemetryPhase(context, telemetry::kPhaseGatewayAccept,
                         TurnTelemetryContext::Clock::now(), TurnTelemetryContext::Clock::now());
    RecordTurnFinished(context, telemetry::kOutcomeSucceeded);
}

} // namespace
} // namespace isla::server::ai_gateway
