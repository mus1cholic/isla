#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

#include "isla/server/evals/eval_json.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "server/src/evals/eval_failure_utils.hpp"
#include "server/src/evals/live_eval_runner.hpp"

namespace isla::server::evals {
namespace {

using isla::server::memory::ParseTimestamp;
using nlohmann::json;
using nlohmann::ordered_json;

constexpr std::string_view kBenchmarkName = "isla_custom_memory";

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

isla::server::memory::Timestamp Ts(std::string_view text) {
    return ParseTimestamp(text);
}

struct BenchmarkCaseDefinition {
    std::string suite_id;
    EvalCase eval_case;
};

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

LiveEvalRunnerConfig BuildLiveEvalRunnerConfig(const IslaCustomMemoryBenchmarkRunConfig& config) {
    return LiveEvalRunnerConfig{
        .host = config.live_gateway_host,
        .port = config.live_gateway_port,
        .path = config.live_gateway_path,
        .operation_timeout = config.live_gateway_operation_timeout,
        .turn_completion_timeout = config.live_gateway_turn_completion_timeout,
    };
}

std::vector<BenchmarkCaseDefinition> BuildBenchmarkCases() {
    return std::vector<BenchmarkCaseDefinition>{
        BenchmarkCaseDefinition{
            .suite_id = "direct_fact_recall",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "direct_fact_recall_tea",
                    .session_id = "isla_custom_direct_fact_recall",
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "Please remember that my favorite tea is genmaicha.",
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "What tea did I say I like?",
                        },
                    .expected_answer = std::string("genmaicha"),
                },
        },
        BenchmarkCaseDefinition{
            .suite_id = "direct_fact_recall",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "direct_fact_recall_language",
                    .session_id = "isla_custom_direct_fact_recall_language",
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "Please remember that my favorite language is Rust.",
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "What language did I say I like most?",
                        },
                    .expected_answer = std::string("Rust"),
                },
        },
        BenchmarkCaseDefinition{
            .suite_id = "recency_contradiction",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "recency_contradiction",
                    .session_id = "isla_custom_recency_contradiction",
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "My preferred editor is Vim.",
                                .create_time = Ts("2026-03-14T09:00:00Z"),
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "Actually, I switched to Neovim last week.",
                                .create_time = Ts("2026-03-16T10:00:00Z"),
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "Which editor am I using now?",
                            .create_time = Ts("2026-03-20T08:30:00Z"),
                        },
                    .expected_answer = std::string("Neovim"),
                },
        },
        BenchmarkCaseDefinition{
            .suite_id = "mid_term_visibility",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "mid_term_visibility",
                    .session_id = "isla_custom_mid_term_visibility",
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text =
                                    "We debugged the Atlas renderer and fixed the culling mode.",
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::Assistant,
                                .text =
                                    "Noted. We debugged the Atlas renderer and fixed the culling "
                                    "mode.",
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "Let's switch topics before the next question.",
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "I am ready for the follow-up question now.",
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "What project were we debugging earlier?",
                        },
                    .expected_answer = std::string("Atlas renderer"),
                },
        },
        BenchmarkCaseDefinition{
            .suite_id = "expandable_exact_detail",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "expandable_exact_detail",
                    .session_id = "isla_custom_expandable_exact_detail",
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text =
                                    "We fixed the failing suite by running bazel test //server/src:eval_runner_test.",
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::Assistant,
                                .text =
                                    "Noted. The failing suite was fixed by running bazel test "
                                    "//server/src:eval_runner_test.",
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "Let's move on for a moment.",
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "What exact command fixed the failing suite?",
                        },
                    .expected_answer =
                        std::string("bazel test //server/src:eval_runner_test"),
                },
        },
        BenchmarkCaseDefinition{
            .suite_id = "relative_timestamp_recall",
            .eval_case =
                EvalCase{
                    .benchmark_name = std::string(kBenchmarkName),
                    .case_id = "relative_timestamp_recall",
                    .session_id = "isla_custom_relative_timestamp_recall",
                    .session_start_time = Ts("2026-03-14T09:59:00Z"),
                    .evaluation_reference_time = Ts("2026-03-20T08:00:00Z"),
                    .conversation =
                        {
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "I booked the Osaka trip today.",
                                .create_time = Ts("2026-03-14T10:00:00Z"),
                            },
                            EvalConversationMessage{
                                .role = isla::server::memory::MessageRole::User,
                                .text = "I also reserved a hotel later that week.",
                                .create_time = Ts("2026-03-18T09:00:00Z"),
                            },
                        },
                    .input =
                        EvalInput{
                            .text = "When did I book the Osaka trip?",
                            .create_time = Ts("2026-03-20T08:30:00Z"),
                        },
                    .expected_answer = std::string("2026-03-14T10:00:00Z"),
                },
        },
    };
}

bool ValidateArtifacts(const BenchmarkCaseDefinition& definition, const EvalArtifacts& artifacts) {
    if (artifacts.status != EvalTurnStatus::kSucceeded) {
        return false;
    }
    if (!artifacts.final_reply.has_value() || artifacts.final_reply->empty()) {
        return false;
    }
    if (!definition.eval_case.expected_answer.has_value() ||
        definition.eval_case.expected_answer->empty()) {
        return true;
    }
    return ToLowerAscii(*artifacts.final_reply)
               .find(ToLowerAscii(*definition.eval_case.expected_answer)) != std::string::npos;
}

ordered_json BuildCaseArtifactJson(const BenchmarkCaseDefinition& definition) {
    ordered_json history = ordered_json::array();
    for (const EvalConversationMessage& message : definition.eval_case.conversation) {
        history.push_back(ordered_json(EvalConversationMessageToJson(message)));
    }

    ordered_json payload = ordered_json::object();
    payload["case_id"] = definition.eval_case.case_id;
    payload["history"] = std::move(history);
    payload["input"] = EvalInputToJson(definition.eval_case.input);
    payload["expected_answer"] = definition.eval_case.expected_answer;
    return payload;
}

ordered_json BuildCaseResultJson(const IslaCustomMemoryCaseReport& case_report) {
    return ordered_json{
        { "passed", case_report.passed },
        { "final_answer_evaluation", case_report.final_answer_evaluation },
        { "final_reply", case_report.final_reply },
        { "failure", FailureToJson(case_report.failure) },
    };
}

ordered_json BuildSuiteArtifactJson(
    std::string_view suite_id, const std::vector<const BenchmarkCaseDefinition*>& definitions,
    const absl::flat_hash_map<std::string, const IslaCustomMemoryCaseReport*>& case_reports) {
    ordered_json cases = ordered_json::array();
    for (const BenchmarkCaseDefinition* definition : definitions) {
        ordered_json case_json = BuildCaseArtifactJson(*definition);
        if (const auto it = case_reports.find(definition->eval_case.case_id);
            it != case_reports.end()) {
            case_json["result"] = BuildCaseResultJson(*it->second);
        }
        cases.push_back(std::move(case_json));
    }

    ordered_json payload = ordered_json::object();
    payload["suite_id"] = std::string(suite_id);
    payload["cases"] = std::move(cases);
    return payload;
}

ordered_json BuildBenchmarkReportJson(const IslaCustomMemoryBenchmarkReport& report) {
    ordered_json cases = ordered_json::array();
    for (const IslaCustomMemoryCaseReport& case_report : report.cases) {
        ordered_json case_json = ordered_json::object();
        case_json["suite_id"] = case_report.suite_id;
        case_json["case_id"] = case_report.case_id;
        case_json["passed"] = case_report.passed;
        case_json["final_answer_evaluation"] = case_report.final_answer_evaluation;
        case_json["final_reply"] = case_report.final_reply;
        case_json["artifact_path"] = case_report.artifact_path;
        case_json["failure"] = FailureToJson(case_report.failure);
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

} // namespace

absl::StatusOr<IslaCustomMemoryBenchmarkReport>
RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig config) {
    std::vector<BenchmarkCaseDefinition> cases = BuildBenchmarkCases();
    if (config.case_id_filter.has_value()) {
        cases.erase(std::remove_if(cases.begin(), cases.end(),
                                   [&config](const BenchmarkCaseDefinition& definition) {
                                       return definition.eval_case.case_id !=
                                              *config.case_id_filter;
                                   }),
                    cases.end());
        if (cases.empty()) {
            return invalid_argument(
                "requested case_id_filter did not match any isla_custom_memory case");
        }
    }

    if (config.output_directory.empty()) {
        config.output_directory = std::filesystem::current_path() / "server" / "src" / "evals" /
                                  "out" / std::string(kBenchmarkName);
    }
    const std::filesystem::path artifacts_directory = config.output_directory / "artifacts";
    std::error_code error;
    std::filesystem::create_directories(artifacts_directory, error);
    if (error) {
        return absl::InternalError("failed to create isla_custom_memory output directory");
    }

    IslaCustomMemoryBenchmarkReport report;
    report.output_directory = DisplayPath(config.output_directory);
    report.total_cases = cases.size();
    report.cases.reserve(cases.size());
    const LiveEvalRunner runner(BuildLiveEvalRunnerConfig(config));

    absl::flat_hash_map<std::string, std::vector<const BenchmarkCaseDefinition*>> suite_definitions;
    for (const BenchmarkCaseDefinition& definition : cases) {
        suite_definitions[definition.suite_id].push_back(&definition);
    }

    for (const BenchmarkCaseDefinition& definition : cases) {
        IslaCustomMemoryCaseReport case_report{
            .suite_id = definition.suite_id,
            .case_id = definition.eval_case.case_id,
        };
        const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(definition.eval_case);
        if (!artifacts.ok()) {
            LOG(WARNING) << kBenchmarkName << " case " << definition.eval_case.case_id
                         << " failed: " << artifacts.status().message();
            case_report.failure = FailureFromStatus(artifacts.status());
            report.failed_cases += 1U;
            report.cases.push_back(std::move(case_report));
            continue;
        }

        if (artifacts->final_reply.has_value() && !artifacts->final_reply->empty()) {
            case_report.final_reply = artifacts->final_reply;
        } else {
            case_report.final_reply = std::nullopt;
        }
        case_report.failure = artifacts->failure;
        case_report.passed = ValidateArtifacts(definition, *artifacts);

        if (case_report.passed) {
            report.passed_cases += 1U;
        } else {
            report.failed_cases += 1U;
        }
        report.cases.push_back(std::move(case_report));
    }

    for (const auto& [suite_id, definitions] : suite_definitions) {
        std::vector<IslaCustomMemoryCaseReport*> suite_case_reports;
        absl::flat_hash_map<std::string, const IslaCustomMemoryCaseReport*> case_reports;
        for (IslaCustomMemoryCaseReport& case_report : report.cases) {
            if (case_report.suite_id != suite_id) {
                continue;
            }
            suite_case_reports.push_back(&case_report);
            case_reports.insert_or_assign(case_report.case_id, &case_report);
        }

        const std::filesystem::path artifact_path = artifacts_directory / (suite_id + ".json");
        const absl::Status write_status = WriteJsonFile(
            artifact_path, BuildSuiteArtifactJson(suite_id, definitions, case_reports));
        if (!write_status.ok()) {
            for (IslaCustomMemoryCaseReport* case_report : suite_case_reports) {
                if (case_report->passed) {
                    report.passed_cases -= 1U;
                    report.failed_cases += 1U;
                }
                case_report->passed = false;
                case_report->artifact_path = std::nullopt;
            }
            continue;
        }

        const std::string display_path = DisplayPath(artifact_path);
        for (IslaCustomMemoryCaseReport* case_report : suite_case_reports) {
            case_report->artifact_path = display_path;
        }
    }

    if (const absl::Status report_status = WriteJsonFile(config.output_directory / "report.json",
                                                         BuildBenchmarkReportJson(report));
        !report_status.ok()) {
        return report_status;
    }
    return report;
}

} // namespace isla::server::evals
