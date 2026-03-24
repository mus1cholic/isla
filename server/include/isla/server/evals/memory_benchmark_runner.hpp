#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/evals/eval_types.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/ollama_llm_client.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::evals {

// A benchmark case with opaque metadata for benchmark-specific fields. Adapters for external
// datasets (LongMemEval, LoCoMo, etc.) normalize their entries into this shape, packing any
// dataset-specific metadata (question_type, answer_session_ids, etc.) into the metadata field.
struct MemoryBenchmarkCase {
    EvalCase eval_case;
    // Opaque benchmark-specific metadata preserved verbatim in per-case artifacts and the
    // aggregate report. The runner does not interpret this field.
    nlohmann::json metadata;
};

// A named collection of benchmark cases from a single dataset. Adapters produce one suite per
// dataset (e.g., LongMemEval, LoCoMo) and the runner uses the benchmark_name for logging, output
// directory naming, and report identification.
struct MemoryBenchmarkSuite {
    std::string benchmark_name;
    std::vector<MemoryBenchmarkCase> cases;
};

struct MemoryBenchmarkCaseReport {
    std::string case_id;
    bool passed = false;
    std::optional<std::string> final_reply;
    std::optional<std::string> expected_answer;
    std::optional<std::string> artifact_path;
    std::optional<EvalFailure> failure;
    // Metadata carried through from the input MemoryBenchmarkCase.
    nlohmann::json metadata;
};

struct MemoryBenchmarkReport {
    std::string benchmark_name;
    std::string output_directory;
    std::size_t total_cases = 0;
    std::size_t passed_cases = 0;
    std::size_t failed_cases = 0;
    std::vector<MemoryBenchmarkCaseReport> cases;
};

struct MemoryBenchmarkRunConfig {
    std::filesystem::path output_directory;
    std::string live_gateway_host = "127.0.0.1";
    std::uint16_t live_gateway_port = 0;
    std::string live_gateway_path = "/";
    std::chrono::milliseconds live_gateway_operation_timeout{ std::chrono::seconds(10) };
    std::chrono::milliseconds live_gateway_turn_completion_timeout{ std::chrono::seconds(60) };
    // Legacy pre-live-gateway knobs retained temporarily for compatibility while callers migrate.
    // RunMemoryBenchmark() rejects non-default values for these fields because the live gateway
    // path no longer applies them locally.
    isla::server::ai_gateway::GatewayLlmRuntimeConfig llm_runtime_config;
    isla::server::OllamaLlmClientConfig ollama_config;
    isla::server::ai_gateway::OpenAiResponsesClientConfig openai_config;
    std::shared_ptr<const isla::server::LlmClient> llm_client;
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> openai_client;
    std::optional<std::size_t> max_rendered_system_prompt_bytes;
    std::optional<std::size_t> max_rendered_working_memory_context_bytes;
    std::optional<std::size_t> max_rendered_prompt_bytes;
    std::shared_ptr<const isla::server::ai_gateway::TelemetrySink> telemetry_sink =
        isla::server::ai_gateway::CreateNoOpTelemetrySink();
};

// Runs a benchmark suite through the live AI gateway serving path. Each case is executed over the
// gateway websocket protocol, so any configured server-side memory store is exercised and can
// persist benchmark sessions. Per-case artifacts plus an aggregate report are written to the
// output directory. A case "passes" when the evaluated turn completes successfully with a non-empty
// reply; actual answer evaluation against expected_answer is deferred to Phase 5 (autoraters).
[[nodiscard]] absl::StatusOr<MemoryBenchmarkReport>
RunMemoryBenchmark(MemoryBenchmarkRunConfig config, const MemoryBenchmarkSuite& suite);

// Builds the aggregate report JSON. Exposed for unit testing the serialization independently.
[[nodiscard]] nlohmann::ordered_json
BuildMemoryBenchmarkReportJson(const MemoryBenchmarkReport& report);

// Converts a case_id into a safe filename stem. Non-alphanumeric characters (except hyphens) are
// replaced with underscores, consecutive underscores are collapsed, and leading/trailing
// underscores are stripped. Returns "unnamed" for empty or all-special-character inputs. Exposed
// for unit testing.
[[nodiscard]] std::string SanitizeCaseIdForFilename(std::string_view case_id);

} // namespace isla::server::evals
