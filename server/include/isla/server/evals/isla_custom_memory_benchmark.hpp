#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server::evals {

struct IslaCustomMemoryCaseReport {
    std::string suite_id;
    std::string case_id;
    bool passed = false;
    std::string final_answer_evaluation = "unimplemented";
    std::optional<std::string> final_reply;
    std::optional<std::string> artifact_path;
};

struct IslaCustomMemoryBenchmarkReport {
    std::string benchmark_name = "isla_custom_memory";
    std::string output_directory;
    std::size_t total_cases = 0;
    std::size_t passed_cases = 0;
    std::size_t failed_cases = 0;
    std::vector<IslaCustomMemoryCaseReport> cases;
};

struct IslaCustomMemoryBenchmarkRunConfig {
    std::filesystem::path output_directory;
    std::optional<std::string> case_id_filter;
    std::shared_ptr<const isla::server::ai_gateway::TelemetrySink> telemetry_sink =
        isla::server::ai_gateway::CreateNoOpTelemetrySink();
};

[[nodiscard]] absl::StatusOr<IslaCustomMemoryBenchmarkReport> RunIslaCustomMemoryBenchmark(
    IslaCustomMemoryBenchmarkRunConfig config = IslaCustomMemoryBenchmarkRunConfig{});

} // namespace isla::server::evals
