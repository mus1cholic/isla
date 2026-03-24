#include "isla/server/evals/memory_benchmark_runner.hpp"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include <nlohmann/json.hpp>

#include "isla/server/evals/eval_json.hpp"
#include "isla/server/evals/eval_runner.hpp"
#include "isla/server/evals/eval_types.hpp"
#include "isla/server/ollama_llm_client.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::CreateOpenAiResponsesClient;
using nlohmann::json;
using nlohmann::ordered_json;
using namespace std::chrono_literals;

struct ResolvedBenchmarkLlm {
    std::shared_ptr<const isla::server::LlmClient> llm_client;
};

absl::StatusOr<ResolvedBenchmarkLlm> ResolveBenchmarkLlmClient(MemoryBenchmarkRunConfig config) {
    if (config.llm_client != nullptr) {
        return ResolvedBenchmarkLlm{
            .llm_client = std::move(config.llm_client),
        };
    }

    if (config.ollama_config.enabled) {
        const absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> created_client =
            isla::server::CreateOllamaLlmClient(config.ollama_config);
        if (!created_client.ok()) {
            return created_client.status();
        }
        return ResolvedBenchmarkLlm{
            .llm_client = *created_client,
        };
    }

    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> openai_client =
        std::move(config.openai_client);
    if (openai_client == nullptr) {
        openai_client = CreateOpenAiResponsesClient(config.openai_config);
    }
    const absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> created_client =
        isla::server::CreateOpenAiLlmClient(openai_client);
    if (!created_client.ok()) {
        return created_client.status();
    }
    return ResolvedBenchmarkLlm{
        .llm_client = *created_client,
    };
}

std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::filesystem::path FindRepositoryRoot(std::filesystem::path start) {
    std::error_code error;
    std::filesystem::path current = std::filesystem::weakly_canonical(std::move(start), error);
    if (error) {
        return std::filesystem::current_path();
    }
    for (;;) {
        if (std::filesystem::exists(current / ".git") ||
            std::filesystem::exists(current / "MODULE.bazel")) {
            return current;
        }
        if (!current.has_parent_path() || current.parent_path() == current) {
            return std::filesystem::current_path();
        }
        current = current.parent_path();
    }
}

std::string DisplayPath(const std::filesystem::path& path) {
    const std::filesystem::path repo_root = FindRepositoryRoot(std::filesystem::current_path());
    std::error_code error;
    std::filesystem::path normalized_path = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        normalized_path = std::filesystem::absolute(path, error);
        if (error) {
            normalized_path = path;
        }
    }
    const std::string current_text = repo_root.generic_string();
    const std::string path_text = normalized_path.generic_string();
    const std::string current_lower = ToLowerAscii(current_text);
    const std::string path_lower = ToLowerAscii(path_text);

    if (path_lower == current_lower) {
        return ".";
    }
    if (path_lower.size() > current_lower.size() &&
        path_lower.compare(0, current_lower.size(), current_lower) == 0 &&
        path_lower[current_lower.size()] == '/') {
        return path_text.substr(current_text.size() + 1U);
    }

    const std::filesystem::path relative = normalized_path.lexically_relative(repo_root);
    if (!relative.empty() && relative.native() != normalized_path.native()) {
        return relative.generic_string();
    }
    return path_text;
}

ordered_json BuildCaseArtifactJson(const MemoryBenchmarkCase& benchmark_case) {
    ordered_json payload = ordered_json::object();
    payload["case_id"] = benchmark_case.eval_case.case_id;
    payload["input"] = EvalInputToJson(benchmark_case.eval_case.input);
    payload["expected_answer"] = benchmark_case.eval_case.expected_answer;
    payload["conversation_length"] = benchmark_case.eval_case.conversation.size();
    if (!benchmark_case.metadata.is_null()) {
        payload["metadata"] = benchmark_case.metadata;
    }
    return payload;
}

} // namespace

std::string SanitizeCaseIdForFilename(std::string_view case_id) {
    std::string result;
    result.reserve(case_id.size());
    for (const char ch : case_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-') {
            result += ch;
        } else {
            // Collapse consecutive underscores.
            if (result.empty() || result.back() != '_') {
                result += '_';
            }
        }
    }
    // Strip leading/trailing underscores.
    const std::size_t start = result.find_first_not_of('_');
    if (start == std::string::npos) {
        return "unnamed";
    }
    const std::size_t end = result.find_last_not_of('_');
    return result.substr(start, end - start + 1U);
}

ordered_json BuildMemoryBenchmarkReportJson(const MemoryBenchmarkReport& report) {
    ordered_json cases = ordered_json::array();
    for (const MemoryBenchmarkCaseReport& case_report : report.cases) {
        ordered_json case_json = ordered_json::object();
        case_json["case_id"] = case_report.case_id;
        case_json["passed"] = case_report.passed;
        case_json["final_reply"] = case_report.final_reply;
        case_json["expected_answer"] = case_report.expected_answer;
        case_json["artifact_path"] = case_report.artifact_path;
        if (!case_report.metadata.is_null()) {
            case_json["metadata"] = case_report.metadata;
        }
        cases.push_back(std::move(case_json));
    }

    ordered_json payload = ordered_json::object();
    payload["benchmark_name"] = report.benchmark_name;
    payload["output_directory"] = report.output_directory;
    payload["total_cases"] = report.total_cases;
    payload["passed_cases"] = report.passed_cases;
    payload["failed_cases"] = report.failed_cases;
    payload["cases"] = std::move(cases);
    return payload;
}

absl::StatusOr<MemoryBenchmarkReport> RunMemoryBenchmark(MemoryBenchmarkRunConfig config,
                                                         const MemoryBenchmarkSuite& suite) {
    if (suite.benchmark_name.empty()) {
        return absl::InvalidArgumentError("benchmark_name must not be empty");
    }
    if (suite.cases.empty()) {
        return absl::InvalidArgumentError("benchmark suite must include at least one case");
    }

    // Validate required per-case fields early so adapters get clear, case-indexed errors.
    for (std::size_t i = 0; i < suite.cases.size(); ++i) {
        const EvalCase& eval_case = suite.cases[i].eval_case;
        if (eval_case.case_id.empty()) {
            return absl::InvalidArgumentError(
                absl::StrCat("case ", i, ": case_id must not be empty"));
        }
        if (eval_case.session_id.empty()) {
            return absl::InvalidArgumentError(absl::StrCat("case ", i, " (", eval_case.case_id,
                                                           "): session_id must not be empty"));
        }
    }

    const absl::StatusOr<ResolvedBenchmarkLlm> resolved_llm = ResolveBenchmarkLlmClient(config);
    if (!resolved_llm.ok()) {
        return resolved_llm.status();
    }
    if (const absl::Status validate_status = resolved_llm->llm_client->Validate();
        !validate_status.ok()) {
        return validate_status;
    }
    if (const absl::Status warmup_status = resolved_llm->llm_client->WarmUp();
        !warmup_status.ok()) {
        LOG(WARNING) << suite.benchmark_name << " benchmark llm warmup failed detail='"
                     << warmup_status.message() << "'";
    }

    // Set up output directory.
    if (config.output_directory.empty()) {
        config.output_directory = std::filesystem::current_path() / "server" / "src" / "evals" /
                                  "out" / suite.benchmark_name;
    }
    const std::filesystem::path artifacts_directory = config.output_directory / "artifacts";
    std::error_code fs_error;
    std::filesystem::create_directories(artifacts_directory, fs_error);
    if (fs_error) {
        return absl::InternalError(
            absl::StrCat("failed to create ", suite.benchmark_name, " output directory"));
    }

    LOG(INFO) << suite.benchmark_name << " benchmark starting cases=" << suite.cases.size()
              << " output=" << DisplayPath(config.output_directory);

    // Run each case through the eval runner.
    MemoryBenchmarkReport report;
    report.benchmark_name = suite.benchmark_name;
    report.output_directory = DisplayPath(config.output_directory);
    report.total_cases = suite.cases.size();
    report.cases.reserve(suite.cases.size());

    for (std::size_t case_idx = 0; case_idx < suite.cases.size(); ++case_idx) {
        const MemoryBenchmarkCase& benchmark_case = suite.cases[case_idx];

        // Ensure benchmark_name is populated from the suite so adapters don't need to set it
        // redundantly on every case.
        EvalCase eval_case = benchmark_case.eval_case;
        eval_case.benchmark_name = suite.benchmark_name;

        LOG(INFO) << suite.benchmark_name << " running case " << (case_idx + 1) << "/"
                  << suite.cases.size() << " id=" << eval_case.case_id
                  << " conversation_turns=" << eval_case.conversation.size();

        isla::server::ai_gateway::GatewayStubResponderConfig responder_config{
            .response_delay = 0ms,
            .async_emit_timeout = 30s,
            .llm_runtime_config = config.llm_runtime_config,
            .llm_client = resolved_llm->llm_client,
        };
        if (config.max_rendered_system_prompt_bytes.has_value()) {
            responder_config.max_rendered_system_prompt_bytes =
                *config.max_rendered_system_prompt_bytes;
        }
        if (config.max_rendered_working_memory_context_bytes.has_value()) {
            responder_config.max_rendered_working_memory_context_bytes =
                *config.max_rendered_working_memory_context_bytes;
        }
        responder_config.max_rendered_prompt_bytes = config.max_rendered_prompt_bytes;

        EvalRunner runner(EvalRunnerConfig{
            .responder_config = std::move(responder_config),
            .telemetry_sink = config.telemetry_sink,
        });

        MemoryBenchmarkCaseReport case_report{
            .case_id = eval_case.case_id,
            .expected_answer = eval_case.expected_answer,
            .metadata = benchmark_case.metadata,
        };

        const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(eval_case);
        if (!artifacts.ok()) {
            LOG(WARNING) << suite.benchmark_name << " case " << eval_case.case_id
                         << " failed: " << artifacts.status().message();
            report.failed_cases += 1U;
            report.cases.push_back(std::move(case_report));
            continue;
        }

        if (artifacts->final_reply.has_value() && !artifacts->final_reply->empty()) {
            case_report.final_reply = artifacts->final_reply;
        }

        // A case "passes" if the turn completed successfully with a non-empty reply. Actual answer
        // evaluation against expected_answer is deferred to Phase 5 (autoraters).
        case_report.passed = (artifacts->status == EvalTurnStatus::kSucceeded) &&
                             case_report.final_reply.has_value();

        // Write per-case artifact file. Sanitize case_id to prevent path traversal or
        // invalid filenames; the original case_id is preserved in the JSON content.
        const std::filesystem::path artifact_path =
            artifacts_directory / (SanitizeCaseIdForFilename(eval_case.case_id) + ".json");
        ordered_json case_artifact = ordered_json::object();
        case_artifact["case"] = BuildCaseArtifactJson(benchmark_case);
        case_artifact["artifacts"] = EvalArtifactsToJson(*artifacts);
        case_artifact["final_reply"] = case_report.final_reply;
        case_artifact["expected_answer"] = eval_case.expected_answer;

        if (const absl::Status write_status = WriteJsonFile(artifact_path, case_artifact);
            !write_status.ok()) {
            LOG(WARNING) << suite.benchmark_name << " failed to write artifact for case "
                         << eval_case.case_id << ": " << write_status.message();
            case_report.passed = false;
        } else {
            case_report.artifact_path = DisplayPath(artifact_path);
        }

        if (case_report.passed) {
            report.passed_cases += 1U;
        } else {
            report.failed_cases += 1U;
        }
        report.cases.push_back(std::move(case_report));
    }

    // Write aggregate report.
    if (const absl::Status report_status = WriteJsonFile(config.output_directory / "report.json",
                                                         BuildMemoryBenchmarkReportJson(report));
        !report_status.ok()) {
        return report_status;
    }

    LOG(INFO) << suite.benchmark_name << " benchmark completed passed=" << report.passed_cases
              << " failed=" << report.failed_cases << " total=" << report.total_cases;

    return report;
}

} // namespace isla::server::evals
