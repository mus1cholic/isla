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
#include "isla/server/gemini_api_embedding_client.hpp"
#include "isla/server/memory/supabase_memory_store.hpp"
#include "isla/server/ollama_llm_client.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int /*unused*/) {
    g_stop_requested = 1;
}

std::string ResolveMainModel(const isla::server::ai_gateway::GatewayLlmRuntimeConfig& config) {
    return config.main_model.empty() ? std::string(isla::server::ai_gateway::kDefaultMainLlmModel)
                                     : config.main_model;
}

std::string
ResolveMidTermFlushDeciderModel(const isla::server::ai_gateway::GatewayLlmRuntimeConfig& config) {
    return config.mid_term_flush_decider_model.empty()
               ? std::string(isla::server::ai_gateway::kDefaultMidTermFlushDeciderModel)
               : config.mid_term_flush_decider_model;
}

std::string
ResolveMidTermCompactorModel(const isla::server::ai_gateway::GatewayLlmRuntimeConfig& config) {
    return config.mid_term_compactor_model.empty()
               ? std::string(isla::server::ai_gateway::kDefaultMidTermCompactorModel)
               : config.mid_term_compactor_model;
}

std::string
ResolveMidTermEmbeddingModel(const isla::server::ai_gateway::GatewayLlmRuntimeConfig& config) {
    return config.mid_term_embedding_model.empty()
               ? std::string(isla::server::ai_gateway::kDefaultMidTermEmbeddingModel)
               : config.mid_term_embedding_model;
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
    std::shared_ptr<const isla::server::LlmClient> llm_client;
    std::shared_ptr<const isla::server::EmbeddingClient> embedding_client;
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
    if (startup_config->ollama_config.enabled) {
        absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> created_ollama_client =
            isla::server::CreateOllamaLlmClient(startup_config->ollama_config);
        if (!created_ollama_client.ok()) {
            LOG(ERROR) << "AI gateway failed to create Ollama llm client detail='"
                       << isla::server::ai_gateway::SanitizeForLog(
                              created_ollama_client.status().message())
                       << "'";
            return 1;
        }
        llm_client = std::move(*created_ollama_client);
        const auto ollama_warmup_start = std::chrono::steady_clock::now();
        const absl::Status ollama_warmup_status = llm_client->WarmUp();
        const auto ollama_warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - ollama_warmup_start)
                                          .count();
        if (ollama_warmup_status.ok()) {
            LOG(INFO) << "AI gateway Ollama connection warmup succeeded duration_ms="
                      << ollama_warmup_ms;
        } else {
            LOG(WARNING) << "AI gateway Ollama connection warmup failed duration_ms="
                         << ollama_warmup_ms << " detail='"
                         << isla::server::ai_gateway::SanitizeForLog(ollama_warmup_status.message())
                         << "'";
        }
    }
    if (llm_client == nullptr && openai_client != nullptr) {
        absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> created_openai_llm_client =
            isla::server::CreateOpenAiLlmClient(openai_client);
        if (!created_openai_llm_client.ok()) {
            LOG(ERROR) << "AI gateway failed to adapt OpenAI responses client to llm client "
                          "detail='"
                       << isla::server::ai_gateway::SanitizeForLog(
                              created_openai_llm_client.status().message())
                       << "'";
            return 1;
        }
        llm_client = std::move(*created_openai_llm_client);
    }

    if (startup_config->gemini_api_embedding_config.enabled) {
        absl::StatusOr<std::shared_ptr<const isla::server::EmbeddingClient>>
            created_embedding_client = isla::server::CreateGeminiApiEmbeddingClient(
                startup_config->gemini_api_embedding_config);
        if (!created_embedding_client.ok()) {
            LOG(ERROR) << "AI gateway failed to create Gemini API embedding client detail='"
                       << isla::server::ai_gateway::SanitizeForLog(
                              created_embedding_client.status().message())
                       << "'";
            return 1;
        }
        embedding_client = std::move(*created_embedding_client);
        if (embedding_client != nullptr) {
            const auto embedding_warmup_start = std::chrono::steady_clock::now();
            const absl::Status embedding_warmup_status = embedding_client->WarmUp();
            const auto embedding_warmup_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - embedding_warmup_start)
                    .count();
            if (embedding_warmup_status.ok()) {
                LOG(INFO) << "AI gateway Gemini API embedding connection warmup succeeded "
                          << "duration_ms=" << embedding_warmup_ms;
            } else {
                LOG(WARNING) << "AI gateway Gemini API embedding connection warmup failed "
                             << "duration_ms=" << embedding_warmup_ms << " detail='"
                             << isla::server::ai_gateway::SanitizeForLog(
                                    embedding_warmup_status.message())
                             << "'";
            }
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
            .llm_runtime_config = startup_config->llm_runtime_config,
            .llm_client = llm_client,
            .openai_config = startup_config->openai_config,
            .openai_client = openai_client,
            .gemini_api_embedding_config = startup_config->gemini_api_embedding_config,
            .embedding_client = embedding_client,
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
    const bool mid_term_memory_configured = responder.IsMidTermMemoryConfigured();
    const bool mid_term_memory_available = responder.IsMidTermMemoryAvailable();
    const absl::Status& mid_term_memory_status = responder.MidTermMemoryInitializationStatus();

    LOG(INFO) << "AI gateway OpenAI config source=" << log_context.config_source
              << " api_key_source=" << log_context.api_key_source << " organization_configured="
              << (log_context.organization_configured ? "true" : "false")
              << " project_configured=" << (log_context.project_configured ? "true" : "false")
              << " mid_term_memory_policy=best_effort"
              << " mid_term_memory_effective_state="
              << (mid_term_memory_available
                      ? "enabled"
                      : (mid_term_memory_configured ? "degraded_working_memory_only"
                                                    : "not_configured"))
              << " mid_term_memory_graceful_degradation=true"
              << " supabase_configured=" << (log_context.supabase_configured ? "true" : "false")
              << " telemetry_logging_enabled="
              << (log_context.telemetry_logging_enabled ? "true" : "false")
              << " telemetry_event_logging_enabled="
              << (log_context.telemetry_event_logging_enabled ? "true" : "false");
    LOG(INFO) << "AI gateway llm models main="
              << isla::server::ai_gateway::SanitizeForLog(
                     ResolveMainModel(startup_config->llm_runtime_config))
              << " mid_term_flush_decider="
              << isla::server::ai_gateway::SanitizeForLog(
                     ResolveMidTermFlushDeciderModel(startup_config->llm_runtime_config))
              << " mid_term_compactor="
              << isla::server::ai_gateway::SanitizeForLog(
                     ResolveMidTermCompactorModel(startup_config->llm_runtime_config))
              << " mid_term_embedding="
              << isla::server::ai_gateway::SanitizeForLog(
                     ResolveMidTermEmbeddingModel(startup_config->llm_runtime_config))
              << " reasoning_effort="
              << isla::server::ai_gateway::TryOpenAiReasoningEffortToString(
                     startup_config->llm_runtime_config.reasoning_effort)
                     .value_or("unknown");
    LOG(INFO) << "AI gateway generic llm provider="
              << (startup_config->ollama_config.enabled ? "ollama"
                  : (llm_client != nullptr)             ? "openai"
                                                        : "disabled");
    if (mid_term_memory_configured && !mid_term_memory_available) {
        LOG(WARNING) << "AI gateway mid-term memory degraded to working-memory-only detail='"
                     << isla::server::ai_gateway::SanitizeForLog(mid_term_memory_status.message())
                     << "'";
    }
    if (startup_config->openai_config.enabled) {
        LOG(INFO) << "AI gateway using OpenAI Responses upstream host="
                  << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.host)
                  << ":" << startup_config->openai_config.port << " scheme="
                  << isla::server::ai_gateway::SanitizeForLog(startup_config->openai_config.scheme)
                  << " timeout_ms=" << startup_config->openai_config.request_timeout.count();
    } else {
        LOG(INFO) << "AI gateway OpenAI Responses upstream disabled";
    }
    if (startup_config->ollama_config.enabled) {
        LOG(INFO) << "AI gateway using Ollama llm base_url="
                  << isla::server::ai_gateway::SanitizeForLog(
                         startup_config->ollama_config.base_url)
                  << " timeout_ms=" << startup_config->ollama_config.request_timeout.count();
    } else {
        LOG(INFO) << "AI gateway Ollama llm disabled";
    }
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
    if (startup_config->gemini_api_embedding_config.enabled) {
        LOG(INFO) << "AI gateway using Gemini API embeddings host="
                  << isla::server::ai_gateway::SanitizeForLog(
                         startup_config->gemini_api_embedding_config.host)
                  << " scheme="
                  << isla::server::ai_gateway::SanitizeForLog(
                         startup_config->gemini_api_embedding_config.scheme)
                  << ":" << startup_config->gemini_api_embedding_config.port << " timeout_ms="
                  << startup_config->gemini_api_embedding_config.request_timeout.count();
    } else {
        LOG(INFO) << "AI gateway Gemini API embeddings disabled";
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
