#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "ai_gateway_websocket_session.hpp"

namespace isla::server::ai_gateway {

class GatewaySessionRegistry;

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
    virtual void OnServerStopping(GatewaySessionRegistry& session_registry) {}
};

using GatewayEmitCallback = std::function<void(absl::Status)>;

class GatewayLiveSession {
  public:
    virtual ~GatewayLiveSession() = default;

    [[nodiscard]] virtual const std::string& session_id() const = 0;
    [[nodiscard]] virtual bool is_closed() const = 0;
    virtual void AsyncEmitTextOutput(std::string turn_id, std::string text,
                                     GatewayEmitCallback on_complete) = 0;
    virtual void AsyncEmitAudioOutput(std::string turn_id, std::string mime_type,
                                      std::string audio_base64,
                                      GatewayEmitCallback on_complete) = 0;
    virtual void AsyncEmitTurnCompleted(std::string turn_id, GatewayEmitCallback on_complete) = 0;
    virtual void AsyncEmitTurnCancelled(std::string turn_id, GatewayEmitCallback on_complete) = 0;
    virtual void AsyncEmitError(std::optional<std::string> turn_id, std::string code,
                                std::string message, GatewayEmitCallback on_complete) = 0;
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
