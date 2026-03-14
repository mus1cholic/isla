#pragma once

#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/memory/supabase_memory_store.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

struct ParsedStartupConfig {
    GatewayServerConfig server_config;
    OpenAiResponsesClientConfig openai_config;
    isla::server::memory::SupabaseMemoryStoreConfig supabase_config;
};

struct StartupLogContext {
    std::string config_source;
    std::string api_key_source;
    bool organization_configured = false;
    bool project_configured = false;
    bool supabase_configured = false;
};

using StartupEnvLookup = std::function<std::optional<std::string>(std::string_view)>;
using StartupEnvMap = std::unordered_map<std::string, std::string>;

[[nodiscard]] absl::StatusOr<StartupEnvMap> LoadDotEnvFile(std::string_view path);
[[nodiscard]] StartupEnvLookup DotEnvFileEnvLookup(std::string_view path);
[[nodiscard]] StartupEnvLookup CombinedStartupEnvLookup(StartupEnvLookup primary,
                                                        StartupEnvLookup fallback);
[[nodiscard]] std::vector<std::filesystem::path>
DefaultDotEnvCandidatePaths(const StartupEnvLookup& env_lookup,
                            const std::filesystem::path& current_path =
                                std::filesystem::current_path());
[[nodiscard]] bool LooksLikeOpenAiProjectId(std::string_view value);
[[nodiscard]] StartupEnvLookup DefaultStartupEnvLookup();
[[nodiscard]] absl::Status ValidateOpenAiStartupConfig(
    const OpenAiResponsesClientConfig& config);
[[nodiscard]] StartupLogContext BuildStartupLogContext(
    int argc, char** argv, const StartupEnvLookup& env_lookup, const ParsedStartupConfig& parsed);
[[nodiscard]] absl::StatusOr<ParsedStartupConfig>
ParseGatewayStartupConfig(int argc, char** argv,
                          const StartupEnvLookup& env_lookup = DefaultStartupEnvLookup());

} // namespace isla::server::ai_gateway
