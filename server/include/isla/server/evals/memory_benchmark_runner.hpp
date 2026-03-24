#pragma once

#include <cstddef>
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
    isla::server::ai_gateway::GatewayLlmRuntimeConfig llm_runtime_config;
    // Optional provider-neutral LLM override used for evaluated turns and
    // mid-term memory helpers. When unset, the runner resolves one from
    // provider config, preferring Ollama over OpenAI to match gateway startup.
    std::shared_ptr<const isla::server::LlmClient> llm_client;
    isla::server::OllamaLlmClientConfig ollama_config;
    isla::server::ai_gateway::OpenAiResponsesClientConfig openai_config;
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> openai_client;
    std::optional<std::size_t> max_rendered_system_prompt_bytes;
    std::optional<std::size_t> max_rendered_working_memory_context_bytes;
    std::optional<std::size_t> max_rendered_prompt_bytes;
    std::shared_ptr<const isla::server::ai_gateway::TelemetrySink> telemetry_sink =
        isla::server::ai_gateway::CreateNoOpTelemetrySink();
};

// Runs a benchmark suite through the app-boundary eval runner. Each case is executed via
// EvalRunner::RunCase(), and per-case artifacts plus an aggregate report are written to the output
// directory. A case "passes" when the evaluated turn completes successfully with a non-empty reply;
// actual answer evaluation against expected_answer is deferred to Phase 5 (autoraters).
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
