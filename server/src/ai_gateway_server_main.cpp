#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <system_error>
#include <thread>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int /*unused*/) {
    g_stop_requested = 1;
}

absl::StatusOr<int> parse_int_argument(std::string_view value, std::string_view name) {
    if (value.empty()) {
        return absl::InvalidArgumentError(std::string(name) + " must not be empty");
    }

    int parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc::invalid_argument || result.ptr != end) {
        return absl::InvalidArgumentError(std::string(name) + " must be a base-10 integer");
    }
    if (result.ec == std::errc::result_out_of_range) {
        return absl::InvalidArgumentError(std::string(name) + " is out of range");
    }
    return parsed;
}

absl::StatusOr<isla::server::ai_gateway::GatewayServerConfig> parse_args(int argc, char** argv) {
    isla::server::ai_gateway::GatewayServerConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--host=", 0) == 0) {
            config.bind_host = argument.substr(7);
            continue;
        }
        if (argument.rfind("--port=", 0) == 0) {
            const absl::StatusOr<int> port = parse_int_argument(argument.substr(7), "port");
            if (!port.ok()) {
                return port.status();
            }
            if (*port < 0 || *port > 65535) {
                return absl::InvalidArgumentError("port must be between 0 and 65535");
            }
            config.port = static_cast<std::uint16_t>(*port);
            continue;
        }
        if (argument.rfind("--backlog=", 0) == 0) {
            const absl::StatusOr<int> backlog = parse_int_argument(argument.substr(10), "backlog");
            if (!backlog.ok()) {
                return backlog.status();
            }
            config.listen_backlog = *backlog;
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

    // TODO(ai-gateway): Replace std::signal with platform-specific signal/control handlers.
    const auto previous_sigint_handler = std::signal(SIGINT, handle_signal);
    const auto previous_sigterm_handler = std::signal(SIGTERM, handle_signal);
    static_cast<void>(previous_sigint_handler);
    static_cast<void>(previous_sigterm_handler);

    isla::server::ai_gateway::GatewayStubResponder responder;
    isla::server::ai_gateway::GatewayServer server(*config, &responder);
    responder.AttachSessionRegistry(&server.session_registry());
    const absl::Status start_status = server.Start();
    if (!start_status.ok()) {
        LOG(ERROR) << "AI gateway failed to start: " << start_status;
        return 1;
    }

    LOG(INFO) << "AI gateway using local Phase-2.5 stub responder";
    LOG(INFO) << "AI gateway listening on " << config->bind_host << ":" << server.bound_port();
    while (g_stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.Stop();
    return 0;
}
