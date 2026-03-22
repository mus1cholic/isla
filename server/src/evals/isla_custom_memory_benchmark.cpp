#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

#include "isla/server/evals/eval_json.hpp"
#include "isla/server/evals/eval_runner.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::CreateOpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesOutputItem;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::memory::ParseTimestamp;
using isla::server::memory::PromptAsset;
using nlohmann::json;
using nlohmann::ordered_json;
using namespace std::chrono_literals;

constexpr std::string_view kBenchmarkName = "isla_custom_memory";

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

isla::server::memory::Timestamp Ts(std::string_view text) {
    return ParseTimestamp(text);
}

struct BenchmarkCaseDefinition {
    std::string suite_id;
    EvalCase eval_case;
    std::vector<std::string> required_prompt_substrings;
    std::vector<std::string> required_mid_term_summaries;
    bool require_expandable_episode = false;
    bool require_evaluation_reference_event = false;
    bool flush_first_exchange = false;
    std::string compacted_tier2_summary;
    std::optional<std::string> compacted_tier1_detail;
    int compacted_salience = 5;
};

absl::Status EmitTextResponse(std::string text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id);

class BenchmarkCaseOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    BenchmarkCaseOpenAiResponsesClient(BenchmarkCaseDefinition definition,
                                       std::string decider_prompt, std::string compactor_prompt,
                                       std::shared_ptr<const OpenAiResponsesClient> live_client)
        : definition_(std::move(definition)), decider_prompt_(std::move(decider_prompt)),
          compactor_prompt_(std::move(compactor_prompt)), live_client_(std::move(live_client)) {}

    [[nodiscard]] absl::Status Validate() const override {
        if (live_client_ == nullptr) {
            return failed_precondition(
                "benchmark live OpenAI client must be configured for evaluated turns");
        }
        return live_client_->Validate();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        if (live_client_ == nullptr) {
            return failed_precondition(
                "benchmark live OpenAI client must be configured for evaluated turns");
        }
        return live_client_->WarmUp();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        if (request.system_prompt == decider_prompt_) {
            const bool should_flush =
                definition_.flush_first_exchange && ((*decider_call_count_)++ == 0);
            return EmitTextResponse(should_flush ? R"json({
                    "should_flush": true,
                    "item_id": "i0",
                    "split_at": null,
                    "reasoning": "Flush the first completed exchange for local benchmark coverage."
                })json"
                                                 : R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No additional flush needed for this benchmark case."
                })json",
                                    on_event,
                                    should_flush ? "resp_decider_flush" : "resp_decider_no_flush");
        }

        if (request.system_prompt == compactor_prompt_) {
            return EmitTextResponse(
                json{
                    { "tier1_detail", definition_.compacted_tier1_detail },
                    { "tier2_summary", definition_.compacted_tier2_summary },
                    { "tier3_ref", definition_.compacted_tier2_summary },
                    { "tier3_keywords",
                      json::array({ "isla", "memory", "benchmark", "mid-term", "phase3" }) },
                    { "salience", definition_.compacted_salience },
                }
                    .dump(),
                on_event, "resp_compactor");
        }

        return live_client_->StreamResponse(request, on_event);
    }

  private:
    BenchmarkCaseDefinition definition_;
    std::string decider_prompt_;
    std::string compactor_prompt_;
    std::shared_ptr<const OpenAiResponsesClient> live_client_;
    std::shared_ptr<int> decider_call_count_ = std::make_shared<int>(0);
};

absl::Status EmitTextResponse(std::string text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id) {
    const absl::Status delta_status =
        on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = std::move(text) });
    if (!delta_status.ok()) {
        return delta_status;
    }
    return on_event(OpenAiResponsesCompletedEvent{ .response_id = std::string(response_id) });
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

std::shared_ptr<const OpenAiResponsesClient>
CreateCaseClient(BenchmarkCaseDefinition definition, std::string decider_prompt,
                 std::string compactor_prompt,
                 std::shared_ptr<const OpenAiResponsesClient> live_client) {
    return std::make_shared<const BenchmarkCaseOpenAiResponsesClient>(
        std::move(definition), std::move(decider_prompt), std::move(compactor_prompt),
        std::move(live_client));
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
            .required_prompt_substrings = { "favorite tea is genmaicha" },
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
            .required_prompt_substrings = { "favorite language is Rust" },
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
            .required_prompt_substrings = { "My preferred editor is Vim.",
                                            "Actually, I switched to Neovim last week." },
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
            .required_mid_term_summaries = {
                "Debugged the Atlas renderer and fixed the culling mode.",
            },
            .flush_first_exchange = true,
            .compacted_tier2_summary = "Debugged the Atlas renderer and fixed the culling mode.",
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
            .required_mid_term_summaries = { "Fixed the failing suite with a bazel test command." },
            .require_expandable_episode = true,
            .flush_first_exchange = true,
            .compacted_tier2_summary = "Fixed the failing suite with a bazel test command.",
            .compacted_tier1_detail = std::string("bazel test //server/src:eval_runner_test"),
            .compacted_salience = 9,
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
            .required_prompt_substrings = { "2026-03-14T10:00:00Z", "2026-03-18T09:00:00Z" },
            .require_evaluation_reference_event = true,
        },
    };
}

bool ValidateArtifacts(const BenchmarkCaseDefinition& definition, const EvalArtifacts& artifacts) {
    if (artifacts.status != EvalTurnStatus::kSucceeded) {
        return false;
    }

    for (const std::string& required : definition.required_prompt_substrings) {
        if (artifacts.prompt.working_memory_context.find(required) == std::string::npos) {
            return false;
        }
    }

    for (const std::string& summary : definition.required_mid_term_summaries) {
        const bool found = std::any_of(artifacts.pre_turn_mid_term_episodes.begin(),
                                       artifacts.pre_turn_mid_term_episodes.end(),
                                       [&summary](const EvalMidTermEpisodeArtifact& episode) {
                                           return episode.tier2_summary == summary;
                                       }) ||
                           std::any_of(artifacts.post_turn_mid_term_episodes.begin(),
                                       artifacts.post_turn_mid_term_episodes.end(),
                                       [&summary](const EvalMidTermEpisodeArtifact& episode) {
                                           return episode.tier2_summary == summary;
                                       });
        if (!found) {
            return false;
        }
    }

    if (definition.require_expandable_episode) {
        const bool has_expandable = std::any_of(artifacts.pre_turn_mid_term_episodes.begin(),
                                                artifacts.pre_turn_mid_term_episodes.end(),
                                                [](const EvalMidTermEpisodeArtifact& episode) {
                                                    return episode.expandable;
                                                }) ||
                                    std::any_of(artifacts.post_turn_mid_term_episodes.begin(),
                                                artifacts.post_turn_mid_term_episodes.end(),
                                                [](const EvalMidTermEpisodeArtifact& episode) {
                                                    return episode.expandable;
                                                });
        if (!has_expandable) {
            return false;
        }
    }

    if (definition.require_evaluation_reference_event) {
        const bool has_reference_time = std::any_of(
            artifacts.replayed_session_history.begin(), artifacts.replayed_session_history.end(),
            [](const EvalReplayEventArtifact& event) {
                return event.kind == EvalReplayEventKind::kEvaluationReferenceTime &&
                       event.timestamp.has_value();
            });
        if (!has_reference_time) {
            return false;
        }
    }

    return true;
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

ordered_json
BuildSuiteArtifactJson(std::string_view suite_id,
                       const std::vector<const BenchmarkCaseDefinition*>& definitions) {
    ordered_json cases = ordered_json::array();
    for (const BenchmarkCaseDefinition* definition : definitions) {
        cases.push_back(BuildCaseArtifactJson(*definition));
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

    const absl::StatusOr<std::string> decider_prompt =
        isla::server::memory::LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    if (!decider_prompt.ok()) {
        return decider_prompt.status();
    }
    const absl::StatusOr<std::string> compactor_prompt =
        isla::server::memory::LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    if (!compactor_prompt.ok()) {
        return compactor_prompt.status();
    }

    auto live_openai_client = CreateOpenAiResponsesClient(config.openai_config);
    if (live_openai_client == nullptr) {
        return failed_precondition("failed to create benchmark live OpenAI client");
    }
    if (const absl::Status validate_status = live_openai_client->Validate();
        !validate_status.ok()) {
        return validate_status;
    }
    if (const absl::Status warmup_status = live_openai_client->WarmUp(); !warmup_status.ok()) {
        LOG(WARNING) << "isla_custom_memory benchmark OpenAI warmup failed detail='"
                     << warmup_status.message() << "'";
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

    absl::flat_hash_map<std::string, std::vector<const BenchmarkCaseDefinition*>> suite_definitions;
    for (const BenchmarkCaseDefinition& definition : cases) {
        suite_definitions[definition.suite_id].push_back(&definition);
    }

    for (const BenchmarkCaseDefinition& definition : cases) {
        EvalRunner runner(EvalRunnerConfig{
            .responder_config =
                isla::server::ai_gateway::GatewayStubResponderConfig{
                    .response_delay = 0ms,
                    .async_emit_timeout = 10s,
                    .llm_runtime_config = config.llm_runtime_config,
                    .openai_config = config.openai_config,
                    .openai_client = CreateCaseClient(definition, *decider_prompt,
                                                      *compactor_prompt, live_openai_client),
                },
            .telemetry_sink = config.telemetry_sink,
            .event_timeout = 10s,
        });

        IslaCustomMemoryCaseReport case_report{
            .suite_id = definition.suite_id,
            .case_id = definition.eval_case.case_id,
        };
        const absl::StatusOr<EvalArtifacts> artifacts = runner.RunCase(definition.eval_case);
        if (!artifacts.ok()) {
            report.failed_cases += 1U;
            report.cases.push_back(std::move(case_report));
            continue;
        }

        if (artifacts->final_reply.has_value() && !artifacts->final_reply->empty()) {
            case_report.final_reply = artifacts->final_reply;
        } else {
            case_report.final_reply = std::nullopt;
        }
        case_report.passed = ValidateArtifacts(definition, *artifacts);

        const std::filesystem::path artifact_path =
            artifacts_directory / (definition.suite_id + ".json");
        const auto suite_it = suite_definitions.find(definition.suite_id);
        const absl::Status write_status =
            suite_it == suite_definitions.end()
                ? absl::InternalError("benchmark suite definitions missing during artifact write")
                : WriteJsonFile(artifact_path,
                                BuildSuiteArtifactJson(definition.suite_id, suite_it->second));
        if (!write_status.ok()) {
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

    if (const absl::Status report_status = WriteJsonFile(config.output_directory / "report.json",
                                                         BuildBenchmarkReportJson(report));
        !report_status.ok()) {
        return report_status;
    }
    return report;
}

} // namespace isla::server::evals
