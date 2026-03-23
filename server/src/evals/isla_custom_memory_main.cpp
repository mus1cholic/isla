#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/initialize.h"
#include "absl/status/statusor.h"
#include "evals/benchmark_cli_utils.hpp"

namespace {

void PrintUsage() {
    std::cout << "Usage: isla_custom_memory_eval [--output_dir=PATH] [--case_id=CASE_ID] "
                 "[gateway startup flags...]\n";
}

} // namespace

int main(int argc, char** argv) {
    absl::InitializeLog();

    isla::server::evals::IslaCustomMemoryBenchmarkRunConfig config;
    std::vector<char*> gateway_argv;
    gateway_argv.reserve(static_cast<std::size_t>(argc));
    gateway_argv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return EXIT_SUCCESS;
        }
        if (const std::optional<std::string_view> output_dir =
                isla::server::evals::ParseFlagValue(arg, "--output_dir=");
            output_dir.has_value()) {
            config.output_directory = std::filesystem::path(*output_dir);
            continue;
        }
        if (const std::optional<std::string_view> case_id =
                isla::server::evals::ParseFlagValue(arg, "--case_id=");
            case_id.has_value()) {
            config.case_id_filter = std::string(*case_id);
            continue;
        }
        gateway_argv.push_back(argv[i]);
    }

    const absl::StatusOr<isla::server::ai_gateway::ParsedStartupConfig> startup_config =
        isla::server::evals::ParseBenchmarkStartupConfig(gateway_argv);
    if (!startup_config.ok()) {
        std::cerr << "Startup config failed: " << startup_config.status() << "\n";
        return EXIT_FAILURE;
    }
    config.llm_runtime_config = startup_config->llm_runtime_config;
    config.openai_config = startup_config->openai_config;

    const absl::StatusOr<isla::server::evals::IslaCustomMemoryBenchmarkReport> report =
        isla::server::evals::RunIslaCustomMemoryBenchmark(std::move(config));
    if (!report.ok()) {
        std::cerr << "Benchmark failed: " << report.status() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Benchmark: " << report->benchmark_name << "\n";
    std::cout << "Output: " << report->output_directory << "\n";
    std::cout << "Passed: " << report->passed_cases << "/" << report->total_cases << "\n";
    for (const isla::server::evals::IslaCustomMemoryCaseReport& case_report : report->cases) {
        std::cout << "- " << case_report.case_id << ": " << (case_report.passed ? "PASS" : "FAIL")
                  << "\n";
    }

    return report->failed_cases == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
