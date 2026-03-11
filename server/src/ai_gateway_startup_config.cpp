#include "ai_gateway_startup_config.hpp"

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "absl/status/status.h"

namespace isla::server::ai_gateway {
namespace {

std::string TrimAscii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string UnquoteValue(std::string_view value) {
    if (value.size() >= 2U && ((value.front() == '"' && value.back() == '"') ||
                               (value.front() == '\'' && value.back() == '\''))) {
        return std::string(value.substr(1U, value.size() - 2U));
    }
    return std::string(value);
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

void ApplyOpenAiEnvDefaults(OpenAiResponsesClientConfig* config,
                            const StartupEnvLookup& env_lookup) {
    if (const std::optional<std::string> api_key = env_lookup("OPENAI_API_KEY");
        api_key.has_value()) {
        config->api_key = *api_key;
    }
    if (const std::optional<std::string> scheme = env_lookup("OPENAI_SCHEME"); scheme.has_value()) {
        config->scheme = *scheme;
    }
    if (const std::optional<std::string> host = env_lookup("OPENAI_HOST"); host.has_value()) {
        config->host = *host;
    }
    if (const std::optional<std::string> target = env_lookup("OPENAI_TARGET"); target.has_value()) {
        config->target = *target;
    }
    if (const std::optional<std::string> organization = env_lookup("OPENAI_ORGANIZATION");
        organization.has_value()) {
        config->organization = *organization;
    }
    if (const std::optional<std::string> project = env_lookup("OPENAI_PROJECT");
        project.has_value()) {
        config->project = *project;
    }
    if (const std::optional<std::string> port = env_lookup("OPENAI_PORT"); port.has_value()) {
        const absl::StatusOr<int> parsed_port = ParseIntArgument(*port, "OPENAI_PORT");
        if (parsed_port.ok() && *parsed_port >= 0 && *parsed_port <= 65535) {
            config->port = static_cast<std::uint16_t>(*parsed_port);
        }
    }
    if (const std::optional<std::string> timeout_ms = env_lookup("OPENAI_TIMEOUT_MS");
        timeout_ms.has_value()) {
        const absl::StatusOr<int> parsed_timeout =
            ParseIntArgument(*timeout_ms, "OPENAI_TIMEOUT_MS");
        if (parsed_timeout.ok() && *parsed_timeout > 0) {
            config->request_timeout = std::chrono::milliseconds(*parsed_timeout);
        }
    }
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
        if (const std::size_t comment = value.find('#'); comment != std::string::npos) {
            const std::string before_comment =
                TrimAscii(std::string_view(value).substr(0, comment));
            value = before_comment;
        }
        values.insert_or_assign(key, UnquoteValue(value));
    }

    return values;
}

StartupEnvLookup DotEnvFileEnvLookup(std::string_view path) {
    const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(path);
    if (!parsed.ok()) {
        return [status = parsed.status()](std::string_view name) -> std::optional<std::string> {
            static_cast<void>(name);
            return std::nullopt;
        };
    }
    const auto shared_values = std::make_shared<const StartupEnvMap>(*parsed);
    return [shared_values](std::string_view name) -> std::optional<std::string> {
        const auto it = shared_values->find(std::string(name));
        if (it == shared_values->end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };
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

StartupEnvLookup DefaultStartupEnvLookup() {
    StartupEnvLookup process_env_lookup = [](std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        const char* value = std::getenv(key.c_str());
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    };
    return CombinedStartupEnvLookup(std::move(process_env_lookup), DotEnvFileEnvLookup(".env"));
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
                                       HasArgumentPrefix(argc, argv, "--openai-project=") ||
                                       HasArgumentPrefix(argc, argv, "--openai-timeout-ms=");
    const bool openai_env_configured = HasNonEmptyEnvVar("OPENAI_API_KEY", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_SCHEME", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_HOST", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_PORT", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_TARGET", env_lookup) ||
                                       HasNonEmptyEnvVar("OPENAI_ORGANIZATION", env_lookup) ||
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
    return context;
}

absl::StatusOr<ParsedStartupConfig> ParseGatewayStartupConfig(int argc, char** argv,
                                                              const StartupEnvLookup& env_lookup) {
    ParsedStartupConfig parsed;
    ApplyOpenAiEnvDefaults(&parsed.openai_config, env_lookup);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.starts_with("--host=")) {
            parsed.server_config.bind_host = argument.substr(7);
            continue;
        }
        if (argument.starts_with("--port=")) {
            const absl::StatusOr<int> port = ParseIntArgument(argument.substr(7), "port");
            if (!port.ok()) {
                return port.status();
            }
            if (*port < 0 || *port > 65535) {
                return absl::InvalidArgumentError("port must be between 0 and 65535");
            }
            parsed.server_config.port = static_cast<std::uint16_t>(*port);
            continue;
        }
        if (argument.starts_with("--backlog=")) {
            const absl::StatusOr<int> backlog = ParseIntArgument(argument.substr(10), "backlog");
            if (!backlog.ok()) {
                return backlog.status();
            }
            parsed.server_config.listen_backlog = *backlog;
            if (parsed.server_config.listen_backlog <= 0) {
                return absl::InvalidArgumentError("backlog must be greater than zero");
            }
            continue;
        }
        if (argument.starts_with("--openai-api-key=")) {
            parsed.openai_config.api_key = argument.substr(17);
            continue;
        }
        if (argument.starts_with("--openai-scheme=")) {
            parsed.openai_config.scheme = argument.substr(16);
            continue;
        }
        if (argument.starts_with("--openai-host=")) {
            parsed.openai_config.host = argument.substr(14);
            continue;
        }
        if (argument.starts_with("--openai-port=")) {
            const absl::StatusOr<int> port = ParseIntArgument(argument.substr(14), "openai-port");
            if (!port.ok()) {
                return port.status();
            }
            if (*port < 0 || *port > 65535) {
                return absl::InvalidArgumentError("openai-port must be between 0 and 65535");
            }
            parsed.openai_config.port = static_cast<std::uint16_t>(*port);
            continue;
        }
        if (argument.starts_with("--openai-target=")) {
            parsed.openai_config.target = argument.substr(16);
            continue;
        }
        if (argument.starts_with("--openai-organization=")) {
            parsed.openai_config.organization = argument.substr(22);
            continue;
        }
        if (argument.starts_with("--openai-project=")) {
            parsed.openai_config.project = argument.substr(17);
            continue;
        }
        if (argument.starts_with("--openai-timeout-ms=")) {
            const absl::StatusOr<int> timeout_ms =
                ParseIntArgument(argument.substr(20), "openai-timeout-ms");
            if (!timeout_ms.ok()) {
                return timeout_ms.status();
            }
            if (*timeout_ms <= 0) {
                return absl::InvalidArgumentError("openai-timeout-ms must be greater than zero");
            }
            parsed.openai_config.request_timeout = std::chrono::milliseconds(*timeout_ms);
            continue;
        }
        return absl::InvalidArgumentError("unknown argument: " + argument);
    }

    parsed.openai_config.enabled = true;
    absl::Status openai_status = ValidateOpenAiStartupConfig(parsed.openai_config);
    if (!openai_status.ok()) {
        return openai_status;
    }
    return parsed;
}

} // namespace isla::server::ai_gateway
