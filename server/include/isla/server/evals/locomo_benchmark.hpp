#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/evals/memory_benchmark_runner.hpp"

namespace isla::server::evals {

struct LoCoMoBenchmarkLoadConfig {
    std::filesystem::path dataset_path;
    std::optional<std::string> case_id_filter;
    double sample_rate = 0.05;
    std::uint32_t random_seed = 0;
};

struct LoCoMoBenchmarkRunConfig {
    std::filesystem::path dataset_path;
    std::filesystem::path output_directory;
    std::optional<std::string> case_id_filter;
    double sample_rate = 0.05;
    std::uint32_t random_seed = 0;
    std::string live_gateway_host = "127.0.0.1";
    std::uint16_t live_gateway_port = 0;
    std::string live_gateway_path = "/";
    std::chrono::milliseconds live_gateway_operation_timeout{ std::chrono::seconds(10) };
    std::chrono::milliseconds live_gateway_turn_completion_timeout{ std::chrono::seconds(60) };
};

// Loads the LoCoMo dataset, expanding each QA annotation into a distinct MemoryBenchmarkCase while
// preserving conversation-level metadata needed for future benchmark-specific analysis.
[[nodiscard]] absl::StatusOr<MemoryBenchmarkSuite>
LoadLoCoMoBenchmarkSuite(LoCoMoBenchmarkLoadConfig config);

// Loads LoCoMo through LoadLoCoMoBenchmarkSuite() and executes it through the generic memory
// benchmark runner.
[[nodiscard]] absl::StatusOr<MemoryBenchmarkReport>
RunLoCoMoBenchmark(LoCoMoBenchmarkRunConfig config);

} // namespace isla::server::evals
