#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    std::cout << "Usage: isla_custom_memory_eval [--output_dir=PATH] [--case_id=CASE_ID] "
                 "[--host=HOST] [--port=PORT]\n"
                 "The AI gateway server must already be running.\n"
                 "Provider/model flags are configured on the running gateway process, not this "
                 "benchmark binary.\n";
}

} // namespace

int main(int argc, char** argv) {
    absl::InitializeLog();

    isla::server::evals::IslaCustomMemoryBenchmarkRunConfig config;
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

        std::cerr << "Unsupported flag for isla_custom_memory_eval: " << arg << "\n";
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
