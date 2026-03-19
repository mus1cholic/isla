#pragma once

#include <gmock/gmock.h>

#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/ai_gateway_websocket_session.hpp"

namespace isla::server::ai_gateway::test {

class MockTelemetrySink : public TelemetrySink {
  public:
    MOCK_METHOD(void, OnTurnAccepted, (const TurnTelemetryContext& context), (const, override));
    MOCK_METHOD(void, OnEvent,
                (const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at),
                (const, override));
    MOCK_METHOD(void, OnPhase,
                (const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at),
                (const, override));
    MOCK_METHOD(void, OnTurnFinished,
                (const TurnTelemetryContext& context, std::string_view outcome,
                 TurnTelemetryContext::Clock::time_point finished_at),
                (const, override));
};

class MockGatewayWebSocketConnection : public GatewayWebSocketConnection {
  public:
    MOCK_METHOD(absl::Status, SendTextFrame, (std::string_view frame), (override));
    MOCK_METHOD(void, Close, (GatewayTransportCloseMode mode), (override));
};

class MockGatewaySessionEventSink : public GatewaySessionEventSink {
  public:
    MOCK_METHOD(void, OnSessionStarted, (const SessionStartedEvent& event), (override));
    MOCK_METHOD(void, OnTurnAccepted, (const TurnAcceptedEvent& event), (override));
    MOCK_METHOD(void, OnTurnCancelRequested, (const TurnCancelRequestedEvent& event), (override));
    MOCK_METHOD(void, OnSessionClosed, (const SessionClosedEvent& event), (override));
};

} // namespace isla::server::ai_gateway::test
