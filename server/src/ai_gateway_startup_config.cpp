#include "ai_gateway_startup_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/log/globals.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ai_gateway_string_utils.hpp"
#include "isla/server/ai_gateway_logging_telemetry_sink.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

std::string UnquoteValue(std::string_view value) {
    if (value.size() >= 2U && ((value.front() == '"' && value.back() == '"') ||
                               (value.front() == '\'' && value.back() == '\''))) {
        return std::string(value.substr(1U, value.size() - 2U));
    }
    return std::string(value);
}

bool IsQuotedValue(std::string_view value) {
    return value.size() >= 2U && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''));
}

bool HasArgumentPrefix(int argc, char** argv, std::string_view prefix) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool HasNonEmptyEnvVar(std::string_view name, const StartupEnvLookup& env_lookup) {
    const std::optional<std::string> value = env_lookup(name);
    return value.has_value() && !value->empty();
}

StartupEnvLookup StartupEnvLookupFromMap(StartupEnvMap values) {
    const auto shared_values = std::make_shared<const StartupEnvMap>(std::move(values));
    return [shared_values](std::string_view name) -> std::optional<std::string> {
        const auto it = shared_values->find(std::string(name));
        if (it == shared_values->end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };
}

bool ParseEnabledFlagValue(std::string_view value) {
    std::string normalized = TrimAscii(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized.empty()) {
        return false;
    }
    return normalized != "0" && normalized != "false" && normalized != "off" && normalized != "no";
}

void AppendIfMissing(std::vector<std::filesystem::path>* paths,
                     const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }
    const auto it = std::find(paths->begin(), paths->end(), candidate);
    if (it == paths->end()) {
        paths->push_back(candidate);
    }
}

absl::StatusOr<int> ParseIntArgument(std::string_view value, std::string_view name) {
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

template <typename ApplyFn>
absl::StatusOr<bool> TryParseIntFlag(std::string_view argument, std::string_view prefix,
                                     std::string_view value_name, ApplyFn&& apply) {
    if (!argument.starts_with(prefix)) {
        return false;
    }

    const absl::StatusOr<int> parsed = ParseIntArgument(argument.substr(prefix.size()), value_name);
    if (!parsed.ok()) {
        return parsed.status();
    }

    const absl::Status apply_status = apply(*parsed);
    if (!apply_status.ok()) {
        return apply_status;
    }
    return true;
}

void ApplyOpenAiEnvDefaults(OpenAiResponsesClientConfig* config,
                            const StartupEnvLookup& env_lookup) {
    bool saw_provider_setting = false;
    if (const std::optional<std::string> api_key = env_lookup("OPENAI_API_KEY");
        api_key.has_value()) {
        config->api_key = *api_key;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> scheme = env_lookup("OPENAI_SCHEME"); scheme.has_value()) {
        config->scheme = *scheme;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> host = env_lookup("OPENAI_HOST"); host.has_value()) {
        config->host = *host;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> target = env_lookup("OPENAI_TARGET"); target.has_value()) {
        config->target = *target;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> organization = env_lookup("OPENAI_ORGANIZATION");
        organization.has_value()) {
        config->organization = *organization;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> project = env_lookup("OPENAI_PROJECT_ID");
        project.has_value()) {
        config->project = *project;
        saw_provider_setting = true;
    } else if (const std::optional<std::string> legacy_project = env_lookup("OPENAI_PROJECT");
               legacy_project.has_value()) {
        config->project = *legacy_project;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> port = env_lookup("OPENAI_PORT"); port.has_value()) {
        saw_provider_setting = true;
        const absl::StatusOr<int> parsed_port = ParseIntArgument(*port, "OPENAI_PORT");
        if (!parsed_port.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid OPENAI_PORT value='"
                         << SanitizeForLog(*port) << "' detail='"
                         << SanitizeForLog(parsed_port.status().message()) << "'";
        } else if (*parsed_port < 0 || *parsed_port > 65535) {
            LOG(WARNING) << "AI gateway ignored out-of-range OPENAI_PORT value='"
                         << SanitizeForLog(*port) << "'";
        } else {
            config->port = static_cast<std::uint16_t>(*parsed_port);
        }
    }
    if (const std::optional<std::string> timeout_ms = env_lookup("OPENAI_TIMEOUT_MS");
        timeout_ms.has_value()) {
        saw_provider_setting = true;
        const absl::StatusOr<int> parsed_timeout =
            ParseIntArgument(*timeout_ms, "OPENAI_TIMEOUT_MS");
        if (!parsed_timeout.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid OPENAI_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "' detail='"
                         << SanitizeForLog(parsed_timeout.status().message()) << "'";
        } else if (*parsed_timeout <= 0) {
            LOG(WARNING) << "AI gateway ignored non-positive OPENAI_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "'";
        } else {
            config->request_timeout = std::chrono::milliseconds(*parsed_timeout);
        }
    }
    if (saw_provider_setting) {
        config->enabled = true;
    }
}

void ApplyOllamaEnvDefaults(isla::server::OllamaLlmClientConfig* config,
                            const StartupEnvLookup& env_lookup) {
    bool saw_provider_setting = false;
    if (const std::optional<std::string> base_url = env_lookup("OLLAMA_BASE_URL");
        base_url.has_value()) {
        config->base_url = *base_url;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> api_key = env_lookup("OLLAMA_API_KEY");
        api_key.has_value()) {
        config->api_key = *api_key;
        saw_provider_setting = true;
    }
    if (const std::optional<std::string> timeout_ms = env_lookup("OLLAMA_TIMEOUT_MS");
        timeout_ms.has_value()) {
        saw_provider_setting = true;
        const absl::StatusOr<int> parsed_timeout =
            ParseIntArgument(*timeout_ms, "OLLAMA_TIMEOUT_MS");
        if (!parsed_timeout.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid OLLAMA_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "' detail='"
                         << SanitizeForLog(parsed_timeout.status().message()) << "'";
        } else if (*parsed_timeout <= 0) {
            LOG(WARNING) << "AI gateway ignored non-positive OLLAMA_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "'";
        } else {
            config->request_timeout = std::chrono::milliseconds(*parsed_timeout);
        }
    }
    if (saw_provider_setting) {
        config->enabled = true;
    }
}

void ApplySupabaseEnvDefaults(isla::server::memory::SupabaseMemoryStoreConfig* config,
                              const StartupEnvLookup& env_lookup) {
    if (const std::optional<std::string> url = env_lookup("SUPABASE_URL"); url.has_value()) {
        config->url = *url;
    }
    if (const std::optional<std::string> key = env_lookup("SUPABASE_SERVICE_ROLE_KEY");
        key.has_value()) {
        config->service_role_key = *key;
    }
    if (const std::optional<std::string> schema = env_lookup("SUPABASE_SCHEMA");
        schema.has_value()) {
        config->schema = *schema;
    }
    if (const std::optional<std::string> timeout_ms = env_lookup("SUPABASE_TIMEOUT_MS");
        timeout_ms.has_value()) {
        const absl::StatusOr<int> parsed_timeout =
            ParseIntArgument(*timeout_ms, "SUPABASE_TIMEOUT_MS");
        if (!parsed_timeout.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid SUPABASE_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "' detail='"
                         << SanitizeForLog(parsed_timeout.status().message()) << "'";
        } else if (*parsed_timeout <= 0) {
            LOG(WARNING) << "AI gateway ignored non-positive SUPABASE_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "'";
        } else {
            config->request_timeout = std::chrono::milliseconds(*parsed_timeout);
        }
    }
    config->enabled = !config->url.empty() || !config->service_role_key.empty();
}

void ApplyGeminiApiEmbeddingEnvDefaults(GeminiApiEmbeddingClientConfig* config,
                                        const StartupEnvLookup& env_lookup) {
    if (const std::optional<std::string> api_key = env_lookup("GEMINI_API_KEY");
        api_key.has_value()) {
        config->api_key = *api_key;
    }
    if (const std::optional<std::string> scheme = env_lookup("GEMINI_API_SCHEME");
        scheme.has_value()) {
        config->scheme = *scheme;
    }
    if (const std::optional<std::string> host = env_lookup("GEMINI_API_HOST"); host.has_value()) {
        config->host = *host;
    }
    if (const std::optional<std::string> port = env_lookup("GEMINI_API_PORT"); port.has_value()) {
        const absl::StatusOr<int> parsed_port = ParseIntArgument(*port, "GEMINI_API_PORT");
        if (!parsed_port.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid GEMINI_API_PORT value='"
                         << SanitizeForLog(*port) << "' detail='"
                         << SanitizeForLog(parsed_port.status().message()) << "'";
        } else if (*parsed_port < 0 || *parsed_port > 65535) {
            LOG(WARNING) << "AI gateway ignored out-of-range GEMINI_API_PORT value='"
                         << SanitizeForLog(*port) << "'";
        } else {
            config->port = static_cast<std::uint16_t>(*parsed_port);
        }
    }
    if (const std::optional<std::string> timeout_ms = env_lookup("GEMINI_API_TIMEOUT_MS");
        timeout_ms.has_value()) {
        const absl::StatusOr<int> parsed_timeout =
            ParseIntArgument(*timeout_ms, "GEMINI_API_TIMEOUT_MS");
        if (!parsed_timeout.ok()) {
            LOG(WARNING) << "AI gateway ignored invalid GEMINI_API_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "' detail='"
                         << SanitizeForLog(parsed_timeout.status().message()) << "'";
        } else if (*parsed_timeout <= 0) {
            LOG(WARNING) << "AI gateway ignored non-positive GEMINI_API_TIMEOUT_MS value='"
                         << SanitizeForLog(*timeout_ms) << "'";
        } else {
            config->request_timeout = std::chrono::milliseconds(*parsed_timeout);
        }
    }
    config->enabled = !config->api_key.empty();
}

void ApplyTelemetryEnvDefaults(ParsedStartupConfig* parsed, const StartupEnvLookup& env_lookup) {
    if (const std::optional<std::string> enabled = env_lookup("AI_GATEWAY_TELEMETRY_LOG");
        enabled.has_value()) {
        parsed->telemetry_logging_enabled = ParseEnabledFlagValue(*enabled);
    }
    if (const std::optional<std::string> log_events = env_lookup("AI_GATEWAY_TELEMETRY_LOG_EVENTS");
        log_events.has_value()) {
        parsed->telemetry_event_logging_enabled = ParseEnabledFlagValue(*log_events);
        parsed->telemetry_logging_enabled =
            parsed->telemetry_logging_enabled || parsed->telemetry_event_logging_enabled;
    }
}

void ApplyLlmRuntimeEnvDefaults(ParsedStartupConfig* parsed, const StartupEnvLookup& env_lookup) {
    if (const std::optional<std::string> main_model = env_lookup("AI_GATEWAY_MAIN_LLM_MODEL");
        main_model.has_value()) {
        parsed->llm_runtime_config.main_model = *main_model;
    }
    if (const std::optional<std::string> decider_model =
            env_lookup("AI_GATEWAY_MID_TERM_FLUSH_DECIDER_MODEL");
        decider_model.has_value()) {
        parsed->llm_runtime_config.mid_term_flush_decider_model = *decider_model;
    }
    if (const std::optional<std::string> compactor_model =
            env_lookup("AI_GATEWAY_MID_TERM_COMPACTOR_MODEL");
        compactor_model.has_value()) {
        parsed->llm_runtime_config.mid_term_compactor_model = *compactor_model;
    }
    if (const std::optional<std::string> embedding_model =
            env_lookup("AI_GATEWAY_MID_TERM_EMBEDDING_MODEL");
        embedding_model.has_value()) {
        parsed->llm_runtime_config.mid_term_embedding_model = *embedding_model;
    }
    if (const std::optional<std::string> reasoning_effort =
            env_lookup("AI_GATEWAY_REASONING_EFFORT");
        reasoning_effort.has_value()) {
        const std::optional<isla::server::ai_gateway::OpenAiReasoningEffort> effort =
            isla::server::ai_gateway::TryParseOpenAiReasoningEffort(*reasoning_effort);
        if (effort.has_value()) {
            parsed->llm_runtime_config.reasoning_effort = *effort;
        }
    }
}

absl::Status ValidateOptionalModelOverride(std::string_view field_name, std::string_view value) {
    if (!value.empty() && TrimAscii(value).empty()) {
        return absl::InvalidArgumentError(std::string(field_name) +
                                          " must not be blank or whitespace-only");
    }
    return absl::OkStatus();
}

absl::Status ValidateLlmRuntimeConfig(const GatewayLlmRuntimeConfig& config) {
    if (const absl::Status status =
            ValidateOptionalModelOverride("main-llm-model", config.main_model);
        !status.ok()) {
        return status;
    }
    if (const absl::Status status = ValidateOptionalModelOverride(
            "mid-term-flush-decider-model", config.mid_term_flush_decider_model);
        !status.ok()) {
        return status;
    }
    if (const absl::Status status = ValidateOptionalModelOverride("mid-term-compactor-model",
                                                                  config.mid_term_compactor_model);
        !status.ok()) {
        return status;
    }
    return ValidateOptionalModelOverride("mid-term-embedding-model",
                                         config.mid_term_embedding_model);
}

} // namespace

absl::StatusOr<StartupEnvMap> LoadDotEnvFile(std::string_view path) {
    std::ifstream input{ std::string(path) };
    if (!input.is_open()) {
        return StartupEnvMap{};
    }

    StartupEnvMap values;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            return absl::InvalidArgumentError("invalid .env line " + std::to_string(line_number) +
                                              ": missing '='");
        }

        const std::string key = TrimAscii(std::string_view(trimmed).substr(0, equals));
        if (key.empty()) {
            return absl::InvalidArgumentError("invalid .env line " + std::to_string(line_number) +
                                              ": empty key");
        }

        std::string value = TrimAscii(std::string_view(trimmed).substr(equals + 1U));
        if (!IsQuotedValue(value)) {
            if (const std::size_t comment = value.find('#'); comment != std::string::npos) {
                const std::string before_comment =
                    TrimAscii(std::string_view(value).substr(0, comment));
                value = before_comment;
            }
        }
        values.insert_or_assign(key, UnquoteValue(value));
    }

    return values;
}

StartupEnvLookup DotEnvFileEnvLookup(std::string_view path) {
    const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(path);
    if (!parsed.ok()) {
        return [](std::string_view name) -> std::optional<std::string> {
            static_cast<void>(name);
            return std::nullopt;
        };
    }
    return StartupEnvLookupFromMap(*parsed);
}

StartupEnvLookup CombinedStartupEnvLookup(StartupEnvLookup primary, StartupEnvLookup fallback) {
    return [primary = std::move(primary),
            fallback = std::move(fallback)](std::string_view name) -> std::optional<std::string> {
        if (primary) {
            const std::optional<std::string> primary_value = primary(name);
            if (primary_value.has_value()) {
                return primary_value;
            }
        }
        if (fallback) {
            return fallback(name);
        }
        return std::nullopt;
    };
}

std::vector<std::filesystem::path>
DefaultDotEnvCandidatePaths(const StartupEnvLookup& env_lookup,
                            const std::filesystem::path& current_path) {
    std::vector<std::filesystem::path> candidates;

    if (const std::optional<std::string> workspace_directory =
            env_lookup("BUILD_WORKSPACE_DIRECTORY");
        workspace_directory.has_value() && !workspace_directory->empty()) {
        AppendIfMissing(&candidates, std::filesystem::path(*workspace_directory) / ".env");
    }

    AppendIfMissing(&candidates, current_path / ".env");
    return candidates;
}

bool LooksLikeOpenAiProjectId(std::string_view value) {
    return value.starts_with("proj_");
}

StartupEnvLookup DefaultStartupEnvLookup() {
    const StartupEnvLookup process_env_lookup =
        [](std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        const char* value = std::getenv(key.c_str());
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    };

    for (const std::filesystem::path& candidate :
         DefaultDotEnvCandidatePaths(process_env_lookup, std::filesystem::current_path())) {
        const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(candidate.string());
        if (!parsed.ok()) {
            LOG(WARNING) << "Ignoring invalid .env file at " << candidate.string() << ": "
                         << parsed.status();
            continue;
        }
        if (!parsed->empty()) {
            VLOG(1) << "Using .env file at " << candidate.string();
            return CombinedStartupEnvLookup(process_env_lookup, StartupEnvLookupFromMap(*parsed));
        }
    }

    VLOG(1) << "No .env file found in startup search path";
    return process_env_lookup;
}

absl::Status ValidateOpenAiStartupConfig(const OpenAiResponsesClientConfig& config) {
    if (config.api_key.empty()) {
        return absl::InvalidArgumentError(
            "missing OpenAI API key; set OPENAI_API_KEY or pass --openai-api-key");
    }
    if (config.host.empty()) {
        return absl::InvalidArgumentError("openai host must not be empty");
    }
    if (config.target.empty() || config.target.front() != '/') {
        return absl::InvalidArgumentError("openai target must start with '/'");
    }
    if (config.scheme != "http" && config.scheme != "https") {
        return absl::InvalidArgumentError("openai scheme must be 'http' or 'https'");
    }
    if (config.request_timeout <= std::chrono::milliseconds::zero()) {
        return absl::InvalidArgumentError("openai timeout must be positive");
    }
    if (config.project.has_value() && !config.project->empty() &&
        !LooksLikeOpenAiProjectId(*config.project)) {
        LOG(WARNING) << "OpenAI project value does not look like a project ID; expected a value "
                        "like 'proj_...', not a display name";
    }
    return absl::OkStatus();
}

StartupLogContext BuildStartupLogContext(int argc, char** argv, const StartupEnvLookup& env_lookup,
                                         const ParsedStartupConfig& parsed) {
    const bool openai_cli_configured = HasArgumentPrefix(argc, argv, "--openai-api-key=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-scheme=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-host=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-port=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-target=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-organization=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-project-id=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-project=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-timeout-ms=");
    const bool openai_env_configured = HasNonEmptyEnvVar("OPENAI_API_KEY", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_SCHEME", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_HOST", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_PORT", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_TARGET", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_ORGANIZATION", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_PROJECT_ID", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_PROJECT", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_TIMEOUT_MS", env_lookup);

    StartupLogContext context;
    context.config_source = openai_cli_configured && openai_env_configured ? "cli+env"
                            : openai_cli_configured                        ? "cli"
                            : openai_env_configured                        ? "env"
                                                                           : "unknown";
    context.api_key_source = HasArgumentPrefix(argc, argv, "--openai-api-key=") ? "cli"
                             : HasNonEmptyEnvVar("OPENAI_API_KEY", env_lookup)  ? "env"
                                                                                : "unknown";
    context.organization_configured = parsed.openai_config.organization.has_value();
    context.project_configured = parsed.openai_config.project.has_value();
    context.supabase_configured = parsed.supabase_config.enabled;
    context.telemetry_logging_enabled = parsed.telemetry_logging_enabled;
    context.telemetry_event_logging_enabled = parsed.telemetry_event_logging_enabled;
    return context;
}

absl::StatusOr<ParsedStartupConfig> ParseGatewayStartupConfig(int argc, char** argv,
                                                              const StartupEnvLookup& env_lookup) {
    ParsedStartupConfig parsed;
    ApplyOpenAiEnvDefaults(&parsed.openai_config, env_lookup);
    ApplyOllamaEnvDefaults(&parsed.ollama_config, env_lookup);
    ApplySupabaseEnvDefaults(&parsed.supabase_config, env_lookup);
    ApplyGeminiApiEmbeddingEnvDefaults(&parsed.gemini_api_embedding_config, env_lookup);
    ApplyTelemetryEnvDefaults(&parsed, env_lookup);
    ApplyLlmRuntimeEnvDefaults(&parsed, env_lookup);
    const auto mark_openai_configured = [&parsed]() { parsed.openai_config.enabled = true; };
    const auto mark_ollama_configured = [&parsed]() { parsed.ollama_config.enabled = true; };

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.starts_with("--host=")) {
            parsed.server_config.bind_host = argument.substr(7);
            continue;
        }
        if (const absl::StatusOr<bool> handled = TryParseIntFlag(
                argument, "--port=", "port",
                [&parsed](int port) -> absl::Status {
                    if (port < 0 || port > 65535) {
                        return absl::InvalidArgumentError("port must be between 0 and 65535");
                    }
                    parsed.server_config.port = static_cast<std::uint16_t>(port);
                    return absl::OkStatus();
                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        if (const absl::StatusOr<bool> handled = TryParseIntFlag(
                argument, "--backlog=", "backlog",
                [&parsed](int backlog) -> absl::Status {
                    if (backlog <= 0) {
                        return absl::InvalidArgumentError("backlog must be greater than zero");
                    }
                    parsed.server_config.listen_backlog = backlog;
                    return absl::OkStatus();
                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        if (argument == "--telemetry-log") {
            parsed.telemetry_logging_enabled = true;
            continue;
        }
        if (argument == "--telemetry-log-events") {
            parsed.telemetry_logging_enabled = true;
            parsed.telemetry_event_logging_enabled = true;
            continue;
        }
        if (argument.starts_with("--openai-api-key=")) {
            parsed.openai_config.api_key = argument.substr(17);
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-scheme=")) {
            parsed.openai_config.scheme = argument.substr(16);
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-host=")) {
            parsed.openai_config.host = argument.substr(14);
            mark_openai_configured();
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--openai-port=", "openai-port",
                                [&parsed](int port) -> absl::Status {
                                    if (port < 0 || port > 65535) {
                                        return absl::InvalidArgumentError(
                                            "openai-port must be between 0 and 65535");
                                    }
                                    parsed.openai_config.port = static_cast<std::uint16_t>(port);
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-target=")) {
            parsed.openai_config.target = argument.substr(16);
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-organization=")) {
            parsed.openai_config.organization = argument.substr(22);
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-project=")) {
            parsed.openai_config.project = argument.substr(17);
            mark_openai_configured();
            continue;
        }
        if (argument.starts_with("--openai-project-id=")) {
            parsed.openai_config.project = argument.substr(20);
            mark_openai_configured();
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--openai-timeout-ms=", "openai-timeout-ms",
                                [&parsed](int timeout_ms) -> absl::Status {
                                    if (timeout_ms <= 0) {
                                        return absl::InvalidArgumentError(
                                            "openai-timeout-ms must be greater than zero");
                                    }
                                    parsed.openai_config.request_timeout =
                                        std::chrono::milliseconds(timeout_ms);
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            mark_openai_configured();
            continue;
        }
        constexpr std::string_view kOllamaBaseUrlPrefix = "--ollama-base-url=";
        if (argument.starts_with(kOllamaBaseUrlPrefix)) {
            parsed.ollama_config.base_url = argument.substr(kOllamaBaseUrlPrefix.size());
            mark_ollama_configured();
            continue;
        }
        constexpr std::string_view kOllamaApiKeyPrefix = "--ollama-api-key=";
        if (argument.starts_with(kOllamaApiKeyPrefix)) {
            parsed.ollama_config.api_key = argument.substr(kOllamaApiKeyPrefix.size());
            mark_ollama_configured();
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--ollama-timeout-ms=", "ollama-timeout-ms",
                                [&parsed](int timeout_ms) -> absl::Status {
                                    if (timeout_ms <= 0) {
                                        return absl::InvalidArgumentError(
                                            "ollama-timeout-ms must be greater than zero");
                                    }
                                    parsed.ollama_config.request_timeout =
                                        std::chrono::milliseconds(timeout_ms);
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            mark_ollama_configured();
            continue;
        }
        constexpr std::string_view kMainLlmModelPrefix = "--main-llm-model=";
        if (argument.starts_with(kMainLlmModelPrefix)) {
            const std::string model = argument.substr(kMainLlmModelPrefix.size());
            if (model.empty()) {
                return absl::InvalidArgumentError("main-llm-model must not be empty");
            }
            parsed.llm_runtime_config.main_model = model;
            continue;
        }
        constexpr std::string_view kMidTermFlushDeciderModelPrefix =
            "--mid-term-flush-decider-model=";
        if (argument.starts_with(kMidTermFlushDeciderModelPrefix)) {
            const std::string model = argument.substr(kMidTermFlushDeciderModelPrefix.size());
            if (model.empty()) {
                return absl::InvalidArgumentError("mid-term-flush-decider-model must not be empty");
            }
            parsed.llm_runtime_config.mid_term_flush_decider_model = model;
            continue;
        }
        constexpr std::string_view kMidTermCompactorModelPrefix = "--mid-term-compactor-model=";
        if (argument.starts_with(kMidTermCompactorModelPrefix)) {
            const std::string model = argument.substr(kMidTermCompactorModelPrefix.size());
            if (model.empty()) {
                return absl::InvalidArgumentError("mid-term-compactor-model must not be empty");
            }
            parsed.llm_runtime_config.mid_term_compactor_model = model;
            continue;
        }
        constexpr std::string_view kMidTermEmbeddingModelPrefix = "--mid-term-embedding-model=";
        if (argument.starts_with(kMidTermEmbeddingModelPrefix)) {
            const std::string model = argument.substr(kMidTermEmbeddingModelPrefix.size());
            if (model.empty()) {
                return absl::InvalidArgumentError("mid-term-embedding-model must not be empty");
            }
            parsed.llm_runtime_config.mid_term_embedding_model = model;
            continue;
        }
        constexpr std::string_view kReasoningEffortPrefix = "--reasoning-effort=";
        if (argument.starts_with(kReasoningEffortPrefix)) {
            const std::string value = argument.substr(kReasoningEffortPrefix.size());
            const std::optional<isla::server::ai_gateway::OpenAiReasoningEffort> effort =
                isla::server::ai_gateway::TryParseOpenAiReasoningEffort(value);
            if (!effort.has_value()) {
                return absl::InvalidArgumentError(
                    "reasoning-effort must be one of: none, minimal, low, medium, high, xhigh");
            }
            parsed.llm_runtime_config.reasoning_effort = *effort;
            continue;
        }
        constexpr std::string_view kGeminiApiKeyPrefix = "--gemini-api-key=";
        if (argument.starts_with(kGeminiApiKeyPrefix)) {
            parsed.gemini_api_embedding_config.api_key =
                argument.substr(kGeminiApiKeyPrefix.size());
            parsed.gemini_api_embedding_config.enabled = true;
            continue;
        }
        constexpr std::string_view kGeminiApiSchemePrefix = "--gemini-api-scheme=";
        if (argument.starts_with(kGeminiApiSchemePrefix)) {
            parsed.gemini_api_embedding_config.scheme =
                argument.substr(kGeminiApiSchemePrefix.size());
            parsed.gemini_api_embedding_config.enabled = true;
            continue;
        }
        constexpr std::string_view kGeminiApiHostPrefix = "--gemini-api-host=";
        if (argument.starts_with(kGeminiApiHostPrefix)) {
            parsed.gemini_api_embedding_config.host = argument.substr(kGeminiApiHostPrefix.size());
            parsed.gemini_api_embedding_config.enabled = true;
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--gemini-api-port=", "gemini-api-port",
                                [&parsed](int port) -> absl::Status {
                                    if (port < 0 || port > 65535) {
                                        return absl::InvalidArgumentError(
                                            "gemini-api-port must be between 0 and 65535");
                                    }
                                    parsed.gemini_api_embedding_config.port =
                                        static_cast<std::uint16_t>(port);
                                    parsed.gemini_api_embedding_config.enabled = true;
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--gemini-api-timeout-ms=", "gemini-api-timeout-ms",
                                [&parsed](int timeout_ms) -> absl::Status {
                                    if (timeout_ms <= 0) {
                                        return absl::InvalidArgumentError(
                                            "gemini-api-timeout-ms must be greater than zero");
                                    }
                                    parsed.gemini_api_embedding_config.request_timeout =
                                        std::chrono::milliseconds(timeout_ms);
                                    parsed.gemini_api_embedding_config.enabled = true;
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        if (argument.starts_with("--supabase-url=")) {
            parsed.supabase_config.url = argument.substr(15);
            parsed.supabase_config.enabled = true;
            continue;
        }
        if (argument.starts_with("--supabase-service-role-key=")) {
            parsed.supabase_config.service_role_key = argument.substr(28);
            parsed.supabase_config.enabled = true;
            continue;
        }
        if (argument.starts_with("--supabase-schema=")) {
            parsed.supabase_config.schema = argument.substr(18);
            continue;
        }
        if (const absl::StatusOr<bool> handled =
                TryParseIntFlag(argument, "--supabase-timeout-ms=", "supabase-timeout-ms",
                                [&parsed](int timeout_ms) -> absl::Status {
                                    if (timeout_ms <= 0) {
                                        return absl::InvalidArgumentError(
                                            "supabase-timeout-ms must be greater than zero");
                                    }
                                    parsed.supabase_config.request_timeout =
                                        std::chrono::milliseconds(timeout_ms);
                                    parsed.supabase_config.enabled = true;
                                    return absl::OkStatus();
                                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        if (const absl::StatusOr<bool> handled = TryParseIntFlag(
                argument, "--verbose=", "verbose",
                [](int level) -> absl::Status {
                    if (level < 0) {
                        return absl::InvalidArgumentError("verbose must be non-negative");
                    }
                    absl::SetGlobalVLogLevel(level);
                    return absl::OkStatus();
                });
            !handled.ok()) {
            return handled.status();
        } else if (*handled) {
            continue;
        }
        return absl::InvalidArgumentError("unknown argument: " + argument);
    }

    if (parsed.openai_config.enabled) {
        absl::Status openai_status = ValidateOpenAiStartupConfig(parsed.openai_config);
        if (!openai_status.ok()) {
            return openai_status;
        }
    }
    if (parsed.ollama_config.enabled) {
        const absl::Status ollama_status =
            isla::server::ValidateOllamaLlmClientConfig(parsed.ollama_config);
        if (!ollama_status.ok()) {
            return ollama_status;
        }
    }
    if (!parsed.openai_config.enabled && !parsed.ollama_config.enabled) {
        return absl::InvalidArgumentError(
            "missing LLM provider config; configure OpenAI or Ollama");
    }
    if (const absl::Status llm_runtime_status = ValidateLlmRuntimeConfig(parsed.llm_runtime_config);
        !llm_runtime_status.ok()) {
        return llm_runtime_status;
    }
    if (parsed.gemini_api_embedding_config.enabled) {
        const absl::Status gemini_embedding_status =
            ValidateGeminiApiEmbeddingClientConfig(parsed.gemini_api_embedding_config);
        if (!gemini_embedding_status.ok()) {
            return gemini_embedding_status;
        }
    }
    if (parsed.supabase_config.enabled) {
        const absl::Status supabase_status =
            isla::server::memory::ValidateSupabaseMemoryStoreConfig(parsed.supabase_config);
        if (!supabase_status.ok()) {
            return supabase_status;
        }
    }
    if (parsed.telemetry_logging_enabled) {
        parsed.server_config.telemetry_sink = CreateLoggingTelemetrySink(LoggingTelemetrySinkConfig{
            .log_events = parsed.telemetry_event_logging_enabled,
        });
    }
    parsed.supabase_config.telemetry_logging_enabled = parsed.telemetry_logging_enabled;
    return parsed;
}

} // namespace isla::server::ai_gateway
