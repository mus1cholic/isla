#include <csignal>
#include <thread>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "ai_gateway_startup_config.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int /*unused*/) {
    g_stop_requested = 1;
}

} // namespace

int main(int argc, char** argv) {
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

    const isla::server::ai_gateway::StartupEnvLookup env_lookup =
        isla::server::ai_gateway::DefaultStartupEnvLookup();
    const absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig> startup_config =
        isla::server::ai_gateway::ParseGatewayStartupConfig(argc, argv, env_lookup);
    if (!startup_config.ok()) {
        LOG(ERROR) << startup_config.status();
        return 1;
    }

    // TODO(ai-gateway): Replace std::signal with platform-specific signal/control handlers.
    const auto previous_sigint_handler = std::signal(SIGINT, handle_signal);
    const auto previous_sigterm_handler = std::signal(SIGTERM, handle_signal);
    static_cast<void>(previous_sigint_handler);
    static_cast<void>(previous_sigterm_handler);

    isla::server::ai_gateway::GatewayStubResponder responder(
        isla::server::ai_gateway::GatewayStubResponderConfig{
            .openai_config = startup_config->openai_config,
        });
    isla::server::ai_gateway::GatewayServer server(startup_config->server_config, &responder);
    responder.AttachSessionRegistry(&server.session_registry());
    const absl::Status start_status = server.Start();
    if (!start_status.ok()) {
        LOG(ERROR) << "AI gateway failed to start: " << start_status;
        return 1;
    }

    const isla::server::ai_gateway::StartupLogContext log_context =
        isla::server::ai_gateway::BuildStartupLogContext(argc, argv, env_lookup, *startup_config);

    LOG(INFO) << "AI gateway OpenAI config source=" << log_context.config_source
              << " api_key_source=" << log_context.api_key_source << " organization_configured="
              << (log_context.organization_configured ? "true" : "false")
              << " project_configured=" << (log_context.project_configured ? "true" : "false");
    LOG(INFO) << "AI gateway using OpenAI Responses upstream host="
              << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.host) << ":"
              << startup_config->openai_config.port << " scheme="
              << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.scheme)
              << " timeout_ms=" << startup_config->openai_config.request_timeout.count();
    LOG(INFO) << "AI gateway listening on " << startup_config->server_config.bind_host << ":"
              << server.bound_port();
    while (g_stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.Stop();
    return 0;
}
