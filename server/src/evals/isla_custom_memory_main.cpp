#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

void PrintUsage() {
    std::cout << "Usage: isla_custom_memory_eval [--output_dir=PATH] [--case_id=CASE_ID]\n";
}

std::optional<std::string_view> ParseFlagValue(std::string_view arg, std::string_view prefix) {
    if (!arg.starts_with(prefix)) {
        return std::nullopt;
    }
    return arg.substr(prefix.size());
}

} // namespace

int main(int argc, char** argv) {
    isla::server::evals::IslaCustomMemoryBenchmarkRunConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return EXIT_SUCCESS;
        }
        if (const std::optional<std::string_view> output_dir = ParseFlagValue(arg, "--output_dir=");
            output_dir.has_value()) {
            config.output_directory = std::filesystem::path(*output_dir);
            continue;
        }
        if (const std::optional<std::string_view> case_id = ParseFlagValue(arg, "--case_id=");
            case_id.has_value()) {
            config.case_id_filter = std::string(*case_id);
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        PrintUsage();
        return EXIT_FAILURE;
    }

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
