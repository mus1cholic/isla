#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/ai_gateway_websocket_session.hpp"

namespace isla::server::ai_gateway {

class GatewaySessionRegistry;
inline constexpr std::size_t kMaxInboundWebSocketMessageBytes = 64U * 1024U;
using GatewayEmitCallback = std::function<void(absl::Status)>;
// Async emit completion reports that the operation ran on the session transport executor and was
// accepted or rejected by the transport boundary; it does not mean bytes have flushed to the
// remote client socket.

struct GatewayServerConfig {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 0;
    int listen_backlog = 8;
    std::chrono::milliseconds shutdown_write_grace_period{ std::chrono::seconds(2) };
    std::shared_ptr<const TelemetrySink> telemetry_sink = CreateNoOpTelemetrySink();
};

class GatewayApplicationEventSink {
  public:
    virtual ~GatewayApplicationEventSink() = default;

    [[nodiscard]] virtual absl::Status HandleSessionStart(const SessionStartRequestEvent& event) {
        static_cast<void>(event);
        return absl::OkStatus();
    }
    virtual void HandleSessionStartAsync(const SessionStartRequestEvent& event,
                                         GatewayEmitCallback on_complete) {
        if (!on_complete) {
            return;
        }
        on_complete(HandleSessionStart(event));
    }
    virtual void OnSessionStarted(const SessionStartedEvent& event) = 0;
    [[nodiscard]] virtual absl::Status HandleTranscriptSeed(const TranscriptSeedEvent& event) {
        static_cast<void>(event);
        return absl::UnimplementedError("transcript seeding is unsupported");
    }
    virtual void OnTurnAccepted(const TurnAcceptedEvent& event) = 0;
    virtual void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) = 0;
    virtual void OnSessionClosed(const SessionClosedEvent& event) = 0;
    virtual void OnServerStopping(GatewaySessionRegistry& session_registry) {}
};

class GatewayLiveSession {
  public:
    virtual ~GatewayLiveSession() = default;

    [[nodiscard]] virtual const std::string& session_id() const = 0;
    [[nodiscard]] virtual bool is_closed() const = 0;
    virtual void AsyncAcceptSessionStart(SessionStartedEvent event,
                                         GatewayEmitCallback on_complete) = 0;
    virtual void AsyncRejectSessionStart(absl::Status status, GatewayEmitCallback on_complete) = 0;
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
    void NotifyServerStopping();

    [[nodiscard]] absl::StatusOr<SessionStartHandlingResult>
    HandleSessionStart(const SessionStartRequestEvent& event) override;
    void OnSessionStarted(const SessionStartedEvent& event) override;
    [[nodiscard]] absl::Status HandleTranscriptSeed(const TranscriptSeedEvent& event) override;
    void OnTurnAccepted(const TurnAcceptedEvent& event) override;
    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override;
    void OnSessionClosed(const SessionClosedEvent& event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Top-level WebSocket gateway server. Owns the listening socket, accepts
// incoming connections, and brokers session lifecycle events between the
// transport layer (GatewayLiveSession/GatewaySessionRegistry) and the
// application layer (GatewayApplicationEventSink, typically the stub responder).
// A single GatewayServer instance is expected per process.
class GatewayServer {
  public:
    // Constructs the server with the given config and application sink. The
    // session_id_generator is pluggable for deterministic IDs in tests; in
    // production the default UUID generator is used. The server is created in
    // the stopped state — call Start() to begin listening.
    explicit GatewayServer(GatewayServerConfig config,
                           GatewayApplicationEventSink* application_sink = nullptr,
                           std::unique_ptr<SessionIdGenerator> session_id_generator =
                               std::make_unique<UuidSessionIdGenerator>());
    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    // Binds to the configured host/port and starts accepting connections on a
    // background I/O thread. Returns OK once the listener is bound and ready
    // (at which point bound_port() reflects the actual port, which matters when
    // the config requested port 0). Returns a non-OK status if the bind/listen
    // fails — the server remains in the stopped state in that case. Calling
    // Start() on an already-running server is an error.
    [[nodiscard]] absl::Status Start();

    // Stops accepting new connections and tears the server down in order:
    //   1. Cancels and closes the listening acceptor so no new sessions are
    //      admitted.
    //   2. Notifies the application sink via OnServerStopping() and asks every
    //      currently-live session to stop (each session drains any in-flight
    //      writes within the configured shutdown_write_grace_period).
    //   3. Joins the accept thread and the session-reaper thread.
    //   4. Joins every remaining live session so no transport callbacks can
    //      fire after Stop() returns.
    //   5. Stops the shared Asio I/O thread.
    // Safe to call multiple times; stopping a server that was never started is
    // a no-op. Safe to call from any application-owned thread (test fixtures,
    // signal handlers, the thread that called Start()).
    //
    // MUST NOT be called from one of the server's own internal threads —
    // specifically the accept thread, the session reaper thread, the shared
    // Asio I/O thread, or any transport callback running on the I/O thread
    // (e.g. from inside a GatewayApplicationEventSink handler). Stop() joins
    // each of those threads unconditionally, so calling it from one of them
    // would self-join and deadlock (or throw on platforms where the runtime
    // catches self-join). If you need to stop the server in response to an
    // event that originates on an internal thread, hand off to an
    // application-owned thread first.
    //
    // IMPORTANT: This is a fully blocking shutdown — the call does not return
    // until every background thread (acceptor, reaper, I/O) and every live
    // session has fully exited. Worst-case wall time is therefore bounded by
    // the slowest live session's drain, which in turn is bounded by
    // shutdown_write_grace_period. Callers that need a time-bounded shutdown
    // should size that config value accordingly.
    void Stop();

    // True between a successful Start() and the corresponding Stop().
    [[nodiscard]] bool is_running() const;
    // Returns the port the listener is bound to. Only meaningful while running;
    // useful after Start() when config.port was 0 (ephemeral port) to discover
    // the assigned port for tests and logging.
    [[nodiscard]] std::uint16_t bound_port() const;
    // Access to the session registry so the application layer can enumerate or
    // look up live sessions (e.g. to push out-of-band events).
    [[nodiscard]] GatewaySessionRegistry& session_registry();
    [[nodiscard]] const GatewaySessionRegistry& session_registry() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::server::ai_gateway
