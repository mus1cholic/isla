#include <chrono>
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
#include "isla/server/memory/supabase_memory_store.hpp"
#include "isla/server/openai_responses_client.hpp"

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
        LOG(ERROR) << "AI gateway startup config parse failed code="
                   << static_cast<int>(startup_config.status().code()) << " detail='"
                   << isla::server::ai_gateway::SanitizeForLog(startup_config.status().message())
                   << "'";
        return 1;
    }

    // TODO(ai-gateway): Replace std::signal with platform-specific signal/control handlers.
    const auto previous_sigint_handler = std::signal(SIGINT, handle_signal);
    const auto previous_sigterm_handler = std::signal(SIGTERM, handle_signal);
    static_cast<void>(previous_sigint_handler);
    static_cast<void>(previous_sigterm_handler);

    absl::StatusOr<isla::server::memory::MemoryStorePtr> memory_store =
        isla::server::memory::CreateSupabaseMemoryStore(startup_config->supabase_config);
    if (!memory_store.ok()) {
        LOG(ERROR) << "AI gateway failed to create Supabase memory store detail='"
                   << isla::server::ai_gateway::SanitizeForLog(memory_store.status().message())
                   << "'";
        return 1;
    }

    // Create the OpenAI client eagerly so we can warm up its transport
    // connection before any client session arrives.
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> openai_client;
    if (startup_config->openai_config.enabled) {
        openai_client =
            isla::server::ai_gateway::CreateOpenAiResponsesClient(startup_config->openai_config);
        const auto openai_warmup_start = std::chrono::steady_clock::now();
        const absl::Status openai_warmup_status = openai_client->WarmUp();
        const auto openai_warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - openai_warmup_start)
                                          .count();
        if (openai_warmup_status.ok()) {
            LOG(INFO) << "AI gateway OpenAI connection warmup succeeded duration_ms="
                      << openai_warmup_ms;
        } else {
            LOG(WARNING) << "AI gateway OpenAI connection warmup failed duration_ms="
                         << openai_warmup_ms << " detail='"
                         << isla::server::ai_gateway::SanitizeForLog(openai_warmup_status.message())
                         << "'";
        }
    }

    // Warm up the Supabase persistent connection.
    if (*memory_store != nullptr) {
        const auto supabase_warmup_start = std::chrono::steady_clock::now();
        const absl::Status supabase_warmup_status = (*memory_store)->WarmUp();
        const auto supabase_warmup_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  supabase_warmup_start)
                .count();
        if (supabase_warmup_status.ok()) {
            LOG(INFO) << "AI gateway Supabase connection warmup succeeded duration_ms="
                      << supabase_warmup_ms;
        } else {
            LOG(WARNING) << "AI gateway Supabase connection warmup failed duration_ms="
                         << supabase_warmup_ms << " detail='"
                         << isla::server::ai_gateway::SanitizeForLog(
                                supabase_warmup_status.message())
                         << "'";
        }
    }

    isla::server::ai_gateway::GatewayStubResponder responder(
        isla::server::ai_gateway::GatewayStubResponderConfig{
            .memory_store = *memory_store,
            .openai_config = startup_config->openai_config,
            .openai_client = openai_client,
        });
    isla::server::ai_gateway::GatewayServer server(startup_config->server_config, &responder);
    responder.AttachSessionRegistry(&server.session_registry());
    const absl::Status start_status = server.Start();
    if (!start_status.ok()) {
        LOG(ERROR) << "AI gateway failed to start code=" << static_cast<int>(start_status.code())
                   << " detail='"
                   << isla::server::ai_gateway::SanitizeForLog(start_status.message()) << "'";
        return 1;
    }

    const isla::server::ai_gateway::StartupLogContext log_context =
        isla::server::ai_gateway::BuildStartupLogContext(argc, argv, env_lookup, *startup_config);

    LOG(INFO) << "AI gateway OpenAI config source=" << log_context.config_source
              << " api_key_source=" << log_context.api_key_source << " organization_configured="
              << (log_context.organization_configured ? "true" : "false")
              << " project_configured=" << (log_context.project_configured ? "true" : "false")
              << " mid_term_memory_policy=enabled"
              << " mid_term_memory_graceful_degradation=true"
              << " supabase_configured=" << (log_context.supabase_configured ? "true" : "false")
              << " telemetry_logging_enabled="
              << (log_context.telemetry_logging_enabled ? "true" : "false")
              << " telemetry_event_logging_enabled="
              << (log_context.telemetry_event_logging_enabled ? "true" : "false");
    LOG(INFO) << "AI gateway mid-term memory model="
              << isla::server::ai_gateway::SanitizeForLog(
                     isla::server::ai_gateway::kDefaultMidTermMemoryModel);
    LOG(INFO) << "AI gateway using OpenAI Responses upstream host="
              << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.host) << ":"
              << startup_config->openai_config.port << " scheme="
              << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.scheme)
              << " timeout_ms=" << startup_config->openai_config.request_timeout.count();
    if (startup_config->supabase_config.enabled) {
        LOG(INFO) << "AI gateway using Supabase memory store url="
                  << isla::server::ai_gateway::SanitizeForLog(startup_config->supabase_config.url)
                  << " schema="
                  << isla::server::ai_gateway::SanitizeForLog(
                         startup_config->supabase_config.schema)
                  << " timeout_ms=" << startup_config->supabase_config.request_timeout.count();
    } else {
        LOG(INFO) << "AI gateway Supabase memory store disabled";
    }
    LOG(INFO) << "AI gateway listening on "
              << isla::server::ai_gateway::SanitizeForLog(startup_config->server_config.bind_host)
              << ":" << server.bound_port();
    while (g_stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.Stop();
    return 0;
}
