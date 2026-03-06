#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "ai_gateway_logging_utils.hpp"
#include "ai_gateway_server.hpp"

namespace {

std::atomic<bool> g_stop_requested{ false };

void handle_signal(int /*unused*/) {
    g_stop_requested.store(true);
}

class LoggingApplicationSink final : public isla::server::ai_gateway::GatewayApplicationEventSink {
  public:
    void OnTurnAccepted(const isla::server::ai_gateway::TurnAcceptedEvent& event) override {
        LOG(INFO) << "AI gateway accepted turn session=" << event.session_id
                  << " turn_id=" << isla::server::ai_gateway::SanitizeForLog(event.turn_id);
    }

    void OnTurnCancelRequested(
        const isla::server::ai_gateway::TurnCancelRequestedEvent& event) override {
        LOG(INFO) << "AI gateway received cancel session=" << event.session_id
                  << " turn_id=" << isla::server::ai_gateway::SanitizeForLog(event.turn_id);
    }

    void OnSessionClosed(const isla::server::ai_gateway::SessionClosedEvent& event) override {
        LOG(INFO) << "AI gateway session closed session=" << event.session_id
                  << " reason=" << static_cast<int>(event.reason) << " detail='"
                  << isla::server::ai_gateway::SanitizeForLog(event.detail) << "'";
    }
};

absl::StatusOr<isla::server::ai_gateway::GatewayServerConfig> parse_args(int argc, char** argv) {
    isla::server::ai_gateway::GatewayServerConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--host=", 0) == 0) {
            config.bind_host = argument.substr(7);
            continue;
        }
        if (argument.rfind("--port=", 0) == 0) {
            const int port = std::atoi(argument.substr(7).c_str());
            if (port < 0 || port > 65535) {
                return absl::InvalidArgumentError("port must be between 0 and 65535");
            }
            config.port = static_cast<std::uint16_t>(port);
            continue;
        }
        if (argument.rfind("--backlog=", 0) == 0) {
            config.listen_backlog = std::atoi(argument.substr(10).c_str());
            if (config.listen_backlog <= 0) {
                return absl::InvalidArgumentError("backlog must be greater than zero");
            }
            continue;
        }
        return absl::InvalidArgumentError("unknown argument: " + argument);
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

    const absl::StatusOr<isla::server::ai_gateway::GatewayServerConfig> config =
        parse_args(argc, argv);
    if (!config.ok()) {
        LOG(ERROR) << config.status();
        return 1;
    }

    const auto previous_sigint_handler = std::signal(SIGINT, handle_signal);
    const auto previous_sigterm_handler = std::signal(SIGTERM, handle_signal);
    static_cast<void>(previous_sigint_handler);
    static_cast<void>(previous_sigterm_handler);

    LoggingApplicationSink sink;
    isla::server::ai_gateway::GatewayServer server(*config, &sink);
    const absl::Status start_status = server.Start();
    if (!start_status.ok()) {
        LOG(ERROR) << "AI gateway failed to start: " << start_status;
        return 1;
    }

    LOG(INFO) << "AI gateway listening on " << config->bind_host << ":" << server.bound_port();
    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.Stop();
    return 0;
}
