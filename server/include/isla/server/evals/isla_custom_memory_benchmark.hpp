#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/evals/eval_types.hpp"

namespace isla::server::evals {

struct IslaCustomMemoryCaseReport {
    std::string suite_id;
    std::string case_id;
    bool passed = false;
    std::string final_answer_evaluation = "unimplemented";
    std::optional<std::string> final_reply;
    std::optional<std::string> artifact_path;
    std::optional<EvalFailure> failure;
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
    std::string live_gateway_host = "127.0.0.1";
    std::uint16_t live_gateway_port = 0;
    std::string live_gateway_path = "/";
    std::chrono::milliseconds live_gateway_operation_timeout{ std::chrono::seconds(10) };
    std::chrono::milliseconds live_gateway_turn_completion_timeout{ std::chrono::seconds(60) };
};

[[nodiscard]] absl::StatusOr<IslaCustomMemoryBenchmarkReport> RunIslaCustomMemoryBenchmark(
    IslaCustomMemoryBenchmarkRunConfig config = IslaCustomMemoryBenchmarkRunConfig{});

} // namespace isla::server::evals
