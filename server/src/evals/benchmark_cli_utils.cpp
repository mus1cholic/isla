#include "evals/benchmark_cli_utils.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace isla::server::evals {

std::optional<std::string_view> ParseFlagValue(std::string_view arg, std::string_view prefix) {
    if (!arg.starts_with(prefix)) {
        return std::nullopt;
    }
    return arg.substr(prefix.size());
}

absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig>
ParseBenchmarkStartupConfig(const std::vector<char*>& gateway_argv,
                            const isla::server::ai_gateway::StartupEnvLookup& env_lookup) {
    std::vector<std::string> argv_storage;
    argv_storage.reserve(gateway_argv.size());
    std::vector<char*> mutable_argv;
    mutable_argv.reserve(gateway_argv.size());

    for (char* argument : gateway_argv) {
        argv_storage.push_back(argument != nullptr ? std::string(argument) : std::string());
        mutable_argv.push_back(argv_storage.back().data());
    }

    return isla::server::ai_gateway::ParseGatewayStartupConfig(
        static_cast<int>(mutable_argv.size()), mutable_argv.data(), env_lookup);
}

absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig>
ParseBenchmarkStartupConfig(const std::vector<char*>& gateway_argv) {
    const isla::server::ai_gateway::StartupEnvLookup env_lookup =
        isla::server::ai_gateway::DefaultStartupEnvLookup();
    return ParseBenchmarkStartupConfig(gateway_argv, env_lookup);
}

} // namespace isla::server::evals
