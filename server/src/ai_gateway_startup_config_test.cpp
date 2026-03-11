#include "ai_gateway_startup_config.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

std::filesystem::path MakeUniqueTestEnvPath(std::string_view stem) {
    static std::atomic<std::uint64_t> sequence{ 0 };

    const std::filesystem::path root = std::filesystem::temp_directory_path();
    const std::uint64_t suffix = sequence.fetch_add(1U);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        root / (std::string(stem) + "_" + std::to_string(now) + "_" + std::to_string(suffix));
    std::filesystem::create_directory(directory);
    return directory / ".env";
}

auto kArg0 = std::to_array("isla_ai_gateway");
auto kHost = std::to_array("--host=0.0.0.0");
auto kPort = std::to_array("--port=8080");
auto kBacklog = std::to_array("--backlog=16");
auto kApiKey = std::to_array("--openai-api-key=cli_key");
auto kScheme = std::to_array("--openai-scheme=http");
auto kOpenAiHost = std::to_array("--openai-host=localhost");
auto kOpenAiPort = std::to_array("--openai-port=8081");
auto kTarget = std::to_array("--openai-target=/custom");
auto kOrg = std::to_array("--openai-organization=org_123");
auto kProject = std::to_array("--openai-project=proj_123");
auto kProjectId = std::to_array("--openai-project-id=proj_456");
auto kTimeout = std::to_array("--openai-timeout-ms=1500");
auto kBadScheme = std::to_array("--openai-scheme=ftp");
auto kBadPort = std::to_array("--openai-port=70000");

TEST(AiGatewayStartupConfigTest, LoadDotEnvFileParsesBasicAssignments) {
    const std::filesystem::path path = MakeUniqueTestEnvPath("isla_ai_gateway_test");
    {
        std::ofstream output(path);
        output << "# comment\n";
        output << "OPENAI_API_KEY=test_key\n";
        output << "OPENAI_HOST = api.test.local\n";
        output << "OPENAI_TARGET=\"/v1/responses\"\n";
    }

    const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(path.string());
    std::filesystem::remove_all(path.parent_path());

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->at("OPENAI_API_KEY"), "test_key");
    EXPECT_EQ(parsed->at("OPENAI_HOST"), "api.test.local");
    EXPECT_EQ(parsed->at("OPENAI_TARGET"), "/v1/responses");
}

TEST(AiGatewayStartupConfigTest, LoadDotEnvFilePreservesHashesInsideQuotedValues) {
    const std::filesystem::path path = MakeUniqueTestEnvPath("isla_ai_gateway_hash");
    {
        std::ofstream output(path);
        output << "OPENAI_API_KEY=\"key#with#hashes\"\n";
        output << "OPENAI_PROJECT_ID='proj_#123'\n";
        output << "OPENAI_HOST=api.openai.com # trailing comment\n";
    }

    const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(path.string());
    std::filesystem::remove_all(path.parent_path());

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->at("OPENAI_API_KEY"), "key#with#hashes");
    EXPECT_EQ(parsed->at("OPENAI_PROJECT_ID"), "proj_#123");
    EXPECT_EQ(parsed->at("OPENAI_HOST"), "api.openai.com");
}

TEST(AiGatewayStartupConfigTest, LoadDotEnvFileRejectsMalformedLine) {
    const std::filesystem::path path = MakeUniqueTestEnvPath("isla_ai_gateway_bad");
    {
        std::ofstream output(path);
        output << "NOT_VALID\n";
    }

    const absl::StatusOr<StartupEnvMap> parsed = LoadDotEnvFile(path.string());
    std::filesystem::remove_all(path.parent_path());

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(AiGatewayStartupConfigTest, CombinedStartupEnvLookupPrefersPrimaryLookup) {
    const StartupEnvLookup combined = CombinedStartupEnvLookup(
        [](std::string_view name) -> std::optional<std::string> {
            if (name == "OPENAI_API_KEY") {
                return "primary_key";
            }
            return std::nullopt;
        },
        [](std::string_view name) -> std::optional<std::string> {
            if (name == "OPENAI_API_KEY") {
                return "fallback_key";
            }
            return std::nullopt;
        });

    const std::optional<std::string> value = combined("OPENAI_API_KEY");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "primary_key");
}

TEST(AiGatewayStartupConfigTest, DefaultDotEnvCandidatePathsPreferWorkspaceDirectory) {
    const std::filesystem::path current =
        std::filesystem::path("C:/tmp/isla/bazel-out/x64_windows-fastbuild/bin");
    const std::vector<std::filesystem::path> candidates = DefaultDotEnvCandidatePaths(
        [](std::string_view name) -> std::optional<std::string> {
            if (name == "BUILD_WORKSPACE_DIRECTORY") {
                return "C:/Users/orion/OneDrive/Desktop/code/isla";
            }
            return std::nullopt;
        },
        current);

    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front(),
              std::filesystem::path("C:/Users/orion/OneDrive/Desktop/code/isla/.env"));
}

TEST(AiGatewayStartupConfigTest,
     DefaultDotEnvCandidatePathsUsesOnlyCurrentDirectoryWithoutWorkspaceOverride) {
    const std::filesystem::path current =
        std::filesystem::path("C:/Users/orion/OneDrive/Desktop/code/isla/server/src");
    const std::vector<std::filesystem::path> candidates = DefaultDotEnvCandidatePaths(
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; }, current);

    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(candidates.front(),
              std::filesystem::path("C:/Users/orion/OneDrive/Desktop/code/isla/server/src/.env"));
}

TEST(AiGatewayStartupConfigTest, LooksLikeOpenAiProjectIdRecognizesExpectedPrefix) {
    EXPECT_TRUE(LooksLikeOpenAiProjectId("proj_123"));
    EXPECT_FALSE(LooksLikeOpenAiProjectId("isla"));
    EXPECT_FALSE(LooksLikeOpenAiProjectId(""));
}

TEST(AiGatewayStartupConfigTest, ParsesCliArgumentsAndOpenAiOverrides) {
    std::array<char*, 12> argv = { kArg0.data(),       kHost.data(),       kPort.data(),
                                   kBacklog.data(),    kApiKey.data(),     kScheme.data(),
                                   kOpenAiHost.data(), kOpenAiPort.data(), kTarget.data(),
                                   kOrg.data(),        kProject.data(),    kTimeout.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      static_cast<void>(name);
                                      return std::nullopt;
                                  });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->server_config.bind_host, "0.0.0.0");
    EXPECT_EQ(parsed->server_config.port, 8080);
    EXPECT_EQ(parsed->server_config.listen_backlog, 16);
    EXPECT_TRUE(parsed->openai_config.enabled);
    EXPECT_EQ(parsed->openai_config.api_key, "cli_key");
    EXPECT_EQ(parsed->openai_config.scheme, "http");
    EXPECT_EQ(parsed->openai_config.host, "localhost");
    EXPECT_EQ(parsed->openai_config.port, 8081);
    EXPECT_EQ(parsed->openai_config.target, "/custom");
    ASSERT_TRUE(parsed->openai_config.organization.has_value());
    EXPECT_EQ(*parsed->openai_config.organization, "org_123");
    ASSERT_TRUE(parsed->openai_config.project.has_value());
    EXPECT_EQ(*parsed->openai_config.project, "proj_123");
    EXPECT_EQ(parsed->openai_config.request_timeout, std::chrono::milliseconds(1500));
}

TEST(AiGatewayStartupConfigTest, UsesEnvironmentDefaultsWhenCliOmitted) {
    std::array<char*, 1> argv = { kArg0.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      if (name == "OPENAI_API_KEY") {
                                          return "env_key";
                                      }
                                      if (name == "OPENAI_HOST") {
                                          return "env.host";
                                      }
                                      if (name == "OPENAI_PORT") {
                                          return "4444";
                                      }
                                      if (name == "OPENAI_TARGET") {
                                          return "/env-target";
                                      }
                                      if (name == "OPENAI_TIMEOUT_MS") {
                                          return "2500";
                                      }
                                      if (name == "OPENAI_PROJECT_ID") {
                                          return "proj_env_123";
                                      }
                                      return std::nullopt;
                                  });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->openai_config.api_key, "env_key");
    EXPECT_EQ(parsed->openai_config.host, "env.host");
    EXPECT_EQ(parsed->openai_config.port, 4444);
    EXPECT_EQ(parsed->openai_config.target, "/env-target");
    EXPECT_EQ(parsed->openai_config.request_timeout, std::chrono::milliseconds(2500));
    ASSERT_TRUE(parsed->openai_config.project.has_value());
    EXPECT_EQ(*parsed->openai_config.project, "proj_env_123");
}

TEST(AiGatewayStartupConfigTest, ParseGatewayStartupConfigAcceptsDotEnvFallback) {
    std::array<char*, 1> argv = { kArg0.data() };
    const StartupEnvLookup lookup = CombinedStartupEnvLookup(
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; },
        [](std::string_view name) -> std::optional<std::string> {
            if (name == "OPENAI_API_KEY") {
                return "dotenv_key";
            }
            return std::nullopt;
        });

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(), lookup);

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->openai_config.api_key, "dotenv_key");
}

TEST(AiGatewayStartupConfigTest, CliOverridesEnvironmentDefaults) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kOpenAiHost.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      if (name == "OPENAI_API_KEY") {
                                          return "env_key";
                                      }
                                      if (name == "OPENAI_HOST") {
                                          return "env.host";
                                      }
                                      return std::nullopt;
                                  });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->openai_config.api_key, "cli_key");
    EXPECT_EQ(parsed->openai_config.host, "localhost");
}

TEST(AiGatewayStartupConfigTest, UsesPreferredCliProjectIdFlag) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kProjectId.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed = ParseGatewayStartupConfig(
        static_cast<int>(argv.size()), argv.data(),
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    ASSERT_TRUE(parsed->openai_config.project.has_value());
    EXPECT_EQ(*parsed->openai_config.project, "proj_456");
}

TEST(AiGatewayStartupConfigTest, UsesLegacyProjectEnvWhenPreferredProjectIdEnvMissing) {
    std::array<char*, 1> argv = { kArg0.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      if (name == "OPENAI_API_KEY") {
                                          return "env_key";
                                      }
                                      if (name == "OPENAI_PROJECT") {
                                          return "proj_legacy_123";
                                      }
                                      return std::nullopt;
                                  });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    ASSERT_TRUE(parsed->openai_config.project.has_value());
    EXPECT_EQ(*parsed->openai_config.project, "proj_legacy_123");
}

TEST(AiGatewayStartupConfigTest, RejectsMissingApiKeyAfterEnvAndCliResolution) {
    std::array<char*, 1> argv = { kArg0.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      static_cast<void>(name);
                                      return std::nullopt;
                                  });

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(parsed.status().message(),
              "missing OpenAI API key; set OPENAI_API_KEY or pass --openai-api-key");
}

TEST(AiGatewayStartupConfigTest, RejectsInvalidOpenAiScheme) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kBadScheme.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      static_cast<void>(name);
                                      return std::nullopt;
                                  });

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(parsed.status().message(), "openai scheme must be 'http' or 'https'");
}

TEST(AiGatewayStartupConfigTest, RejectsInvalidOpenAiPort) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kBadPort.data() };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(),
                                  [](std::string_view name) -> std::optional<std::string> {
                                      static_cast<void>(name);
                                      return std::nullopt;
                                  });

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(parsed.status().message(), "openai-port must be between 0 and 65535");
}

TEST(AiGatewayStartupConfigTest, BuildStartupLogContextReportsCliOnlySource) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kOrg.data() };
    const absl::StatusOr<ParsedStartupConfig> parsed = ParseGatewayStartupConfig(
        static_cast<int>(argv.size()), argv.data(),
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; });

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    const StartupLogContext context = BuildStartupLogContext(
        static_cast<int>(argv.size()), argv.data(),
        [](std::string_view) -> std::optional<std::string> { return std::nullopt; }, *parsed);

    EXPECT_EQ(context.config_source, "cli");
    EXPECT_EQ(context.api_key_source, "cli");
    EXPECT_TRUE(context.organization_configured);
    EXPECT_FALSE(context.project_configured);
}

TEST(AiGatewayStartupConfigTest, BuildStartupLogContextReportsEnvOnlySource) {
    std::array<char*, 1> argv = { kArg0.data() };
    const auto env_lookup = [](std::string_view name) -> std::optional<std::string> {
        if (name == "OPENAI_API_KEY") {
            return "env_key";
        }
        if (name == "OPENAI_PROJECT") {
            return "env_project";
        }
        return std::nullopt;
    };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(), env_lookup);

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    const StartupLogContext context =
        BuildStartupLogContext(static_cast<int>(argv.size()), argv.data(), env_lookup, *parsed);

    EXPECT_EQ(context.config_source, "env");
    EXPECT_EQ(context.api_key_source, "env");
    EXPECT_FALSE(context.organization_configured);
    EXPECT_TRUE(context.project_configured);
}

TEST(AiGatewayStartupConfigTest, BuildStartupLogContextReportsMixedSourceWhenCliOverridesEnv) {
    std::array<char*, 3> argv = { kArg0.data(), kApiKey.data(), kProject.data() };
    const auto env_lookup = [](std::string_view name) -> std::optional<std::string> {
        if (name == "OPENAI_API_KEY") {
            return "env_key";
        }
        if (name == "OPENAI_HOST") {
            return "env.host";
        }
        return std::nullopt;
    };

    const absl::StatusOr<ParsedStartupConfig> parsed =
        ParseGatewayStartupConfig(static_cast<int>(argv.size()), argv.data(), env_lookup);

    ASSERT_TRUE(parsed.ok()) << parsed.status();
    const StartupLogContext context =
        BuildStartupLogContext(static_cast<int>(argv.size()), argv.data(), env_lookup, *parsed);

    EXPECT_EQ(context.config_source, "cli+env");
    EXPECT_EQ(context.api_key_source, "cli");
    EXPECT_FALSE(context.organization_configured);
    EXPECT_TRUE(context.project_configured);
}

} // namespace
} // namespace isla::server::ai_gateway
