#include "evals/benchmark_cli_utils.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace isla::server::evals {
namespace {

auto kArg0 = std::to_array("benchmark_eval");
auto kApiKey = std::to_array("--openai-api-key=cli_key");
auto kMainLlmModel = std::to_array("--main-llm-model=gpt-4.1-mini");

TEST(BenchmarkCliUtilsTest, ParseFlagValueReturnsSuffixWhenPrefixMatches) {
    const std::optional<std::string_view> value =
        ParseFlagValue("--output_dir=/tmp/out", "--output_dir=");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "/tmp/out");
}

TEST(BenchmarkCliUtilsTest, ParseFlagValueReturnsNulloptWhenPrefixDoesNotMatch) {
    EXPECT_FALSE(ParseFlagValue("--seed=5", "--output_dir=").has_value());
}

TEST(BenchmarkCliUtilsTest, ParseBenchmarkStartupConfigUsesProvidedEnvLookup) {
    std::vector<char*> argv{ kArg0.data(), kMainLlmModel.data() };

    const absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig> parsed =
        ParseBenchmarkStartupConfig(argv, [](std::string_view name) -> std::optional<std::string> {
            if (name == "OPENAI_API_KEY") {
                return "env_key";
            }
            return std::nullopt;
        });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->openai_config.api_key, "env_key");
    EXPECT_EQ(parsed->llm_runtime_config.main_model, "gpt-4.1-mini");
}

TEST(BenchmarkCliUtilsTest, ParseBenchmarkStartupConfigAcceptsCliApiKey) {
    std::vector<char*> argv{ kArg0.data(), kApiKey.data() };

    const absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig> parsed =
        ParseBenchmarkStartupConfig(
            argv, [](std::string_view) -> std::optional<std::string> { return std::nullopt; });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->openai_config.api_key, "cli_key");
}

} // namespace
} // namespace isla::server::evals
