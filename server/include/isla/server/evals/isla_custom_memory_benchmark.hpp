#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/ollama_llm_client.hpp"
#include "isla/server/openai_responses_client.hpp"

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
    std::string live_gateway_host = "127.0.0.1";
    std::uint16_t live_gateway_port = 0;
    std::string live_gateway_path = "/";
    std::chrono::milliseconds live_gateway_operation_timeout{ std::chrono::seconds(10) };
    std::chrono::milliseconds live_gateway_turn_completion_timeout{ std::chrono::seconds(60) };
    isla::server::ai_gateway::GatewayLlmRuntimeConfig llm_runtime_config;
    isla::server::OllamaLlmClientConfig ollama_config;
    isla::server::ai_gateway::OpenAiResponsesClientConfig openai_config;
    std::shared_ptr<const isla::server::LlmClient> live_llm_client;
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> live_openai_client;
    std::shared_ptr<const isla::server::ai_gateway::TelemetrySink> telemetry_sink =
        isla::server::ai_gateway::CreateNoOpTelemetrySink();
};

[[nodiscard]] absl::StatusOr<IslaCustomMemoryBenchmarkReport> RunIslaCustomMemoryBenchmark(
    IslaCustomMemoryBenchmarkRunConfig config = IslaCustomMemoryBenchmarkRunConfig{});

} // namespace isla::server::evals
