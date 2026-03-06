#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "ai_gateway_session_handler.hpp"
#include "isla/shared/ai_gateway_session.hpp"

namespace isla::server::ai_gateway {

enum class SessionCloseReason {
    ProtocolEnded = 0,
    TransportClosed,
    TransportError,
    SendFailed,
    ServerStopping,
};

struct SessionClosedEvent {
    std::string session_id;
    bool session_started = false;
    std::optional<std::string> inflight_turn_id;
    SessionCloseReason reason = SessionCloseReason::TransportClosed;
    std::string detail;
};

class GatewayWebSocketConnection {
  public:
    virtual ~GatewayWebSocketConnection() = default;

    [[nodiscard]] virtual absl::Status SendTextFrame(std::string_view frame) = 0;
    virtual void Close() = 0;
};

class GatewaySessionEventSink {
  public:
    virtual ~GatewaySessionEventSink() = default;

    virtual void OnTurnAccepted(const TurnAcceptedEvent& event) = 0;
    virtual void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) = 0;
    virtual void OnSessionClosed(const SessionClosedEvent& event) = 0;
};

class SessionIdGenerator {
  public:
    virtual ~SessionIdGenerator() = default;
    [[nodiscard]] virtual std::string NextSessionId() = 0;
};

class SequentialSessionIdGenerator final : public SessionIdGenerator {
  public:
    explicit SequentialSessionIdGenerator(std::string prefix = "srv_");

    [[nodiscard]] std::string NextSessionId() override;

  private:
    std::string prefix_;
    std::atomic<std::uint64_t> next_id_{ 1 };
};

class GatewayWebSocketSessionAdapter {
  public:
    GatewayWebSocketSessionAdapter(std::string session_id, GatewayWebSocketConnection& connection,
                                   GatewaySessionEventSink* event_sink = nullptr);

    [[nodiscard]] absl::Status HandleIncomingTextFrame(std::string_view frame);
    [[nodiscard]] absl::Status HandleTransportError(std::string_view message);
    void HandleTransportClosed();
    void HandleServerShutdown();

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    [[nodiscard]] bool is_closed() const {
        return closed_;
    }

    [[nodiscard]] const isla::shared::ai_gateway::SessionSnapshot& snapshot() const {
        return handler_.snapshot();
    }

  private:
    [[nodiscard]] absl::Status SendFrames(const std::vector<std::string>& frames);
    void CloseSession(SessionCloseReason reason, std::string_view detail,
                      std::optional<std::string> inflight_turn_id, bool close_transport);
    [[nodiscard]] std::optional<std::string> active_turn_id() const;

    std::string session_id_;
    GatewayWebSocketConnection& connection_;
    GatewaySessionEventSink* event_sink_ = nullptr;
    GatewaySessionHandler handler_;
    bool closed_ = false;
};

class GatewayWebSocketSessionFactory {
  public:
    explicit GatewayWebSocketSessionFactory(
        std::unique_ptr<SessionIdGenerator> session_id_generator);

    [[nodiscard]] std::unique_ptr<GatewayWebSocketSessionAdapter>
    CreateSession(GatewayWebSocketConnection& connection,
                  GatewaySessionEventSink* event_sink = nullptr);

  private:
    std::unique_ptr<SessionIdGenerator> session_id_generator_;
};

} // namespace isla::server::ai_gateway
