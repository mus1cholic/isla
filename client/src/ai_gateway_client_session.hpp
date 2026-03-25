#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "isla/shared/ai_gateway_protocol.hpp"

namespace isla::client {

using AiGatewayMessageCallback =
    std::function<void(const isla::shared::ai_gateway::GatewayMessage&)>;
using AiGatewayTransportClosedCallback = std::function<void(absl::Status)>;

struct AiGatewayClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string path = "/";
    std::chrono::milliseconds operation_timeout{ std::chrono::seconds(2) };
    AiGatewayMessageCallback on_message;
    AiGatewayTransportClosedCallback on_transport_closed;
};

class AiGatewayClientSession {
  public:
    explicit AiGatewayClientSession(AiGatewayClientConfig config = {});
    ~AiGatewayClientSession();

    AiGatewayClientSession(const AiGatewayClientSession&) = delete;
    AiGatewayClientSession& operator=(const AiGatewayClientSession&) = delete;

    [[nodiscard]] absl::Status
    ConnectAndStart(std::optional<std::string> client_session_id = std::nullopt);
    [[nodiscard]] absl::Status SendTranscriptSeed(std::string turn_id, std::string role,
                                                  std::string text);
    [[nodiscard]] absl::Status SendTextInput(std::string turn_id, std::string text);
    [[nodiscard]] absl::Status RequestTurnCancel(std::string turn_id);
    [[nodiscard]] absl::Status EndSession();
    void Close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::optional<std::string> session_id() const;

  private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace isla::client
