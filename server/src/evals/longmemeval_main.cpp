#include "isla/server/evals/longmemeval_benchmark.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "absl/log/initialize.h"
#include "benchmark_cli_utils.hpp"

namespace {

std::optional<std::uint16_t> ParsePort(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    int parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < 0 || parsed > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

void PrintUsage() {
    std::cout << "Usage: longmemeval_eval --dataset=PATH [--output_dir=PATH] [--case_id=CASE_ID] "
                 "[--sample_rate=RATE] [--seed=SEED] [--host=HOST] [--port=PORT]\n"
                 "The AI gateway server must already be running.\n"
                 "Provider/model flags are configured on the running gateway process, not this "
                 "benchmark binary.\n";
}

} // namespace

int main(int argc, char** argv) {
    absl::InitializeLog();

    isla::server::evals::LongMemEvalBenchmarkRunConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return EXIT_SUCCESS;
        }
        if (const std::optional<std::string_view> dataset =
                isla::server::evals::ParseFlagValue(arg, "--dataset=");
            dataset.has_value()) {
            config.dataset_path = std::filesystem::path(*dataset);
            continue;
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
        if (const std::optional<std::string_view> sample_rate =
                isla::server::evals::ParseFlagValue(arg, "--sample_rate=");
            sample_rate.has_value()) {
            char* end_ptr = nullptr;
            const double parsed = std::strtod(std::string(*sample_rate).c_str(), &end_ptr);
            if (end_ptr == nullptr || *end_ptr != '\0' || !std::isfinite(parsed) || parsed <= 0.0 ||
                parsed > 1.0) {
                std::cerr << "Invalid --sample_rate value: " << *sample_rate << "\n";
                PrintUsage();
                return EXIT_FAILURE;
            }
            config.sample_rate = parsed;
            continue;
        }
        if (const std::optional<std::string_view> seed =
                isla::server::evals::ParseFlagValue(arg, "--seed=");
            seed.has_value()) {
            std::uint32_t parsed = 0;
            const char* begin = seed->data();
            const char* end = seed->data() + seed->size();
            const std::from_chars_result result = std::from_chars(begin, end, parsed);
            if (result.ec != std::errc() || result.ptr != end) {
                std::cerr << "Invalid --seed value: " << *seed << "\n";
                PrintUsage();
                return EXIT_FAILURE;
            }
            config.random_seed = parsed;
            continue;
        }
        if (const std::optional<std::string_view> host =
                isla::server::evals::ParseFlagValue(arg, "--host=");
            host.has_value()) {
            config.live_gateway_host = std::string(*host);
            continue;
        }
        if (const std::optional<std::string_view> port =
                isla::server::evals::ParseFlagValue(arg, "--port=");
            port.has_value()) {
            const std::optional<std::uint16_t> parsed_port = ParsePort(*port);
            if (!parsed_port.has_value()) {
                std::cerr << "Invalid --port value: " << *port << "\n";
                PrintUsage();
                return EXIT_FAILURE;
            }
            config.live_gateway_port = *parsed_port;
            continue;
        }

        std::cerr << "Unsupported flag for longmemeval_eval: " << arg << "\n";
        PrintUsage();
        return EXIT_FAILURE;
    }

    if (config.dataset_path.empty()) {
        std::cerr << "Error: --dataset is required.\n";
        PrintUsage();
        return EXIT_FAILURE;
    }

    const absl::StatusOr<isla::server::evals::MemoryBenchmarkReport> report =
        isla::server::evals::RunLongMemEvalBenchmark(std::move(config));
    if (!report.ok()) {
        std::cerr << "Benchmark failed: " << report.status() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Benchmark: " << report->benchmark_name << "\n";
    std::cout << "Output: " << report->output_directory << "\n";
    std::cout << "Passed: " << report->passed_cases << "/" << report->total_cases << "\n";
    for (const isla::server::evals::MemoryBenchmarkCaseReport& case_report : report->cases) {
        std::cout << "- " << case_report.case_id << ": " << (case_report.passed ? "PASS" : "FAIL")
                  << "\n";
    }

    return report->failed_cases == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
