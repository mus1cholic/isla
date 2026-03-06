#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "ai_gateway_websocket_session.hpp"

namespace isla::server::ai_gateway {

struct GatewayServerConfig {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 0;
    int listen_backlog = 8;
};

class GatewayApplicationEventSink {
  public:
    virtual ~GatewayApplicationEventSink() = default;

    virtual void OnTurnAccepted(const TurnAcceptedEvent& event) = 0;
    virtual void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) = 0;
    virtual void OnSessionClosed(const SessionClosedEvent& event) = 0;
};

class GatewayLiveSession {
  public:
    virtual ~GatewayLiveSession() = default;

    [[nodiscard]] virtual const std::string& session_id() const = 0;
    [[nodiscard]] virtual bool is_closed() const = 0;
    [[nodiscard]] virtual absl::Status EmitTextOutput(std::string_view turn_id,
                                                      std::string_view text) = 0;
    [[nodiscard]] virtual absl::Status EmitAudioOutput(std::string_view turn_id,
                                                       std::string_view mime_type,
                                                       std::string_view audio_base64) = 0;
    [[nodiscard]] virtual absl::Status EmitTurnCompleted(std::string_view turn_id) = 0;
    [[nodiscard]] virtual absl::Status EmitTurnCancelled(std::string_view turn_id) = 0;
    [[nodiscard]] virtual absl::Status EmitError(std::optional<std::string_view> turn_id,
                                                 std::string_view code,
                                                 std::string_view message) = 0;
};

class GatewaySessionRegistry final : public GatewaySessionEventSink {
  public:
    explicit GatewaySessionRegistry(GatewayApplicationEventSink* application_sink = nullptr);
    ~GatewaySessionRegistry();

    GatewaySessionRegistry(const GatewaySessionRegistry&) = delete;
    GatewaySessionRegistry& operator=(const GatewaySessionRegistry&) = delete;

    void RegisterSession(const std::shared_ptr<GatewayLiveSession>& session);
    [[nodiscard]] std::shared_ptr<GatewayLiveSession>
    FindSession(std::string_view session_id) const;
    [[nodiscard]] std::size_t SessionCount() const;

    void OnTurnAccepted(const TurnAcceptedEvent& event) override;
    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override;
    void OnSessionClosed(const SessionClosedEvent& event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class GatewayServer {
  public:
    explicit GatewayServer(GatewayServerConfig config,
                           GatewayApplicationEventSink* application_sink = nullptr,
                           std::unique_ptr<SessionIdGenerator> session_id_generator =
                               std::make_unique<SequentialSessionIdGenerator>());
    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    [[nodiscard]] absl::Status Start();
    void Stop();

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] GatewaySessionRegistry& session_registry();
    [[nodiscard]] const GatewaySessionRegistry& session_registry() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::server::ai_gateway
