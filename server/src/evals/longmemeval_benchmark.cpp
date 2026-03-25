#include "isla/server/evals/longmemeval_benchmark.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include <nlohmann/json.hpp>

#include "isla/server/evals/benchmark_adapter_utils.hpp"
#include "isla/server/evals/eval_runner.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::memory::FormatTimestamp;
using isla::server::memory::Timestamp;
using nlohmann::json;

constexpr std::string_view kBenchmarkName = "longmemeval_s";

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::StatusOr<MemoryBenchmarkCase> ParseBenchmarkCase(const json& entry, std::size_t case_index) {
    if (!entry.is_object()) {
        return invalid_argument(
            absl::StrCat("LongMemEval entry ", case_index, " must be a JSON object"));
    }

    const std::string entry_context = absl::StrCat("LongMemEval entry ", case_index);
    const absl::StatusOr<std::string> question_id =
        GetRequiredStringField(entry, "question_id", entry_context);
    if (!question_id.ok()) {
        return question_id.status();
    }
    const std::string case_context = absl::StrCat("LongMemEval entry ", *question_id);

    const absl::StatusOr<std::string> question_type =
        GetRequiredStringField(entry, "question_type", case_context);
    if (!question_type.ok()) {
        return question_type.status();
    }
    const absl::StatusOr<std::string> question =
        GetRequiredStringField(entry, "question", case_context);
    if (!question.ok()) {
        return question.status();
    }
    const absl::StatusOr<std::string> question_date_text =
        GetRequiredStringField(entry, "question_date", case_context);
    if (!question_date_text.ok()) {
        return question_date_text.status();
    }
    const absl::StatusOr<Timestamp> question_time =
        ParseBenchmarkTimestamp(*question_date_text, "LongMemEval question_date");
    if (!question_time.ok()) {
        return question_time.status();
    }

    const absl::StatusOr<const json*> answer_it = GetRequiredField(entry, "answer", case_context);
    if (!answer_it.ok()) {
        return answer_it.status();
    }
    const absl::StatusOr<std::string> expected_answer =
        NormalizeStringOrIntegerValue(**answer_it, "LongMemEval answer");
    if (!expected_answer.ok()) {
        return expected_answer.status();
    }

    const absl::StatusOr<std::vector<std::string>> haystack_session_ids =
        GetRequiredStringArrayField(entry, "haystack_session_ids", case_context);
    if (!haystack_session_ids.ok()) {
        return haystack_session_ids.status();
    }
    const absl::StatusOr<std::vector<std::string>> answer_session_ids =
        GetRequiredStringArrayField(entry, "answer_session_ids", case_context);
    if (!answer_session_ids.ok()) {
        return answer_session_ids.status();
    }
    const absl::StatusOr<std::vector<std::string>> haystack_date_texts =
        GetRequiredStringArrayField(entry, "haystack_dates", case_context);
    if (!haystack_date_texts.ok()) {
        return haystack_date_texts.status();
    }

    const absl::StatusOr<const json*> sessions_it =
        GetRequiredField(entry, "haystack_sessions", case_context);
    if (!sessions_it.ok()) {
        return sessions_it.status();
    }
    if (!(**sessions_it).is_array()) {
        return invalid_argument("LongMemEval field 'haystack_sessions' must be an array");
    }

    if (haystack_session_ids->size() != haystack_date_texts->size() ||
        haystack_session_ids->size() != (**sessions_it).size()) {
        return invalid_argument(absl::StrCat(
            "LongMemEval entry ", *question_id,
            " has mismatched haystack_session_ids, haystack_dates, and haystack_sessions lengths"));
    }

    std::size_t total_turns = 0;
    for (const auto& session_turns : **sessions_it) {
        if (session_turns.is_array()) {
            total_turns += session_turns.size();
        }
    }

    std::vector<EvalConversationMessage> conversation;
    conversation.reserve(total_turns);
    std::vector<std::string> normalized_haystack_dates;
    normalized_haystack_dates.reserve((**sessions_it).size());
    std::optional<Timestamp> session_start_time;
    json has_answer_locations = json::array();
    std::size_t flattened_turn_index = 0U;

    for (std::size_t session_index = 0; session_index < (**sessions_it).size(); ++session_index) {
        const absl::StatusOr<Timestamp> session_time = ParseBenchmarkTimestamp(
            (*haystack_date_texts)[session_index], "LongMemEval haystack_dates entry");
        if (!session_time.ok()) {
            return session_time.status();
        }
        normalized_haystack_dates.push_back(FormatTimestamp(*session_time));
        if (!session_start_time.has_value() || *session_time < *session_start_time) {
            session_start_time = *session_time;
        }

        const json& session_turns = (**sessions_it)[session_index];
        if (!session_turns.is_array()) {
            return invalid_argument(absl::StrCat("LongMemEval haystack_sessions[", session_index,
                                                 "] must be an array of turns"));
        }

        for (std::size_t turn_index = 0; turn_index < session_turns.size(); ++turn_index) {
            const json& turn = session_turns[turn_index];
            if (!turn.is_object()) {
                return invalid_argument(absl::StrCat("LongMemEval turn at session ", session_index,
                                                     " index ", turn_index,
                                                     " must be a JSON object"));
            }
            const std::string turn_context =
                absl::StrCat("LongMemEval turn at session ", session_index, " index ", turn_index);
            const absl::StatusOr<std::string> role_text =
                GetRequiredStringField(turn, "role", turn_context);
            if (!role_text.ok()) {
                return role_text.status();
            }
            const absl::StatusOr<std::string> content =
                GetRequiredStringField(turn, "content", turn_context);
            if (!content.ok()) {
                return content.status();
            }
            const absl::StatusOr<isla::server::memory::MessageRole> role =
                ParseBenchmarkConversationRole(*role_text, "LongMemEval turn role");
            if (!role.ok()) {
                return role.status();
            }

            conversation.push_back(EvalConversationMessage{
                .role = *role,
                .text = *content,
                .create_time = *session_time,
            });

            const auto has_answer_it = turn.find("has_answer");
            if (has_answer_it != turn.end()) {
                if (!has_answer_it->is_boolean()) {
                    return invalid_argument(absl::StrCat("LongMemEval has_answer at session ",
                                                         session_index, " turn ", turn_index,
                                                         " must be a boolean"));
                }
                if (has_answer_it->get<bool>()) {
                    has_answer_locations.push_back(json{
                        { "session_index", session_index },
                        { "session_id", (*haystack_session_ids)[session_index] },
                        { "turn_index", turn_index },
                        { "flattened_turn_index", flattened_turn_index },
                    });
                }
            }
            ++flattened_turn_index;
        }
    }

    const absl::StatusOr<EvalCase> eval_case =
        BuildEvalCaseFromBenchmarkTimeline(EvalBenchmarkTimelineCase{
            .benchmark_name = std::string(kBenchmarkName),
            .case_id = *question_id,
            .session_id = absl::StrCat(kBenchmarkName, "_", *question_id),
            .session_start_time = session_start_time,
            .evaluation_reference_time = *question_time,
            .conversation = std::move(conversation),
            .input =
                EvalInput{
                    .text = *question,
                    .create_time = *question_time,
                },
            .expected_answer = *expected_answer,
        });
    if (!eval_case.ok()) {
        return eval_case.status();
    }

    return MemoryBenchmarkCase{
        .eval_case = *eval_case,
        .metadata =
            json{
                { "question_type", *question_type },
                { "answer_session_ids", *answer_session_ids },
                { "haystack_session_ids", *haystack_session_ids },
                { "haystack_dates", std::move(normalized_haystack_dates) },
                { "question_date", FormatTimestamp(*question_time) },
                { "has_answer_turn_locations", std::move(has_answer_locations) },
            },
    };
}

absl::Status ValidateLoadConfig(const LongMemEvalBenchmarkLoadConfig& config) {
    if (const absl::Status path_status =
            ValidateBenchmarkDatasetPath(config.dataset_path, "LongMemEval");
        !path_status.ok()) {
        return path_status;
    }
    if (const absl::Status sample_rate_status =
            ValidateBenchmarkSampleRate(config.sample_rate, "LongMemEval");
        !sample_rate_status.ok()) {
        return sample_rate_status;
    }
    return absl::OkStatus();
}

} // namespace

absl::StatusOr<MemoryBenchmarkSuite>
LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig config) {
    if (const absl::Status status = ValidateLoadConfig(config); !status.ok()) {
        return status;
    }

    const absl::StatusOr<json> dataset =
        ReadJsonFile(config.dataset_path, "LongMemEval dataset JSON");
    if (!dataset.ok()) {
        return dataset.status();
    }
    if (!dataset->is_array()) {
        return invalid_argument("LongMemEval dataset root must be a JSON array");
    }

    MemoryBenchmarkSuite suite;
    suite.benchmark_name = std::string(kBenchmarkName);
    suite.cases.reserve(dataset->size());

    for (std::size_t index = 0; index < dataset->size(); ++index) {
        absl::StatusOr<MemoryBenchmarkCase> parsed_case =
            ParseBenchmarkCase((*dataset)[index], index);
        if (!parsed_case.ok()) {
            return parsed_case.status();
        }
        suite.cases.push_back(std::move(*parsed_case));
    }

    if (config.case_id_filter.has_value()) {
        FilterCasesById(&suite.cases, *config.case_id_filter,
                        [](const MemoryBenchmarkCase& benchmark_case) -> std::string_view {
                            return benchmark_case.eval_case.case_id;
                        });
        if (suite.cases.empty()) {
            return invalid_argument("requested case_id_filter did not match any LongMemEval case");
        }
        return suite;
    }

    ApplyDeterministicSampling(&suite.cases, config.sample_rate, config.random_seed);
    if (suite.cases.empty()) {
        return invalid_argument(
            "LongMemEval sampling removed all cases; raise sample_rate or use case_id_filter");
    }

    return suite;
}

absl::StatusOr<MemoryBenchmarkReport>
RunLongMemEvalBenchmark(LongMemEvalBenchmarkRunConfig config) {
    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = std::move(config.dataset_path),
            .case_id_filter = std::move(config.case_id_filter),
            .sample_rate = config.sample_rate,
            .random_seed = config.random_seed,
        });
    if (!suite.ok()) {
        return suite.status();
    }

    return RunMemoryBenchmark(
        MemoryBenchmarkRunConfig{
            .output_directory = std::move(config.output_directory),
            .live_gateway_host = std::move(config.live_gateway_host),
            .live_gateway_port = config.live_gateway_port,
            .live_gateway_path = std::move(config.live_gateway_path),
            .live_gateway_operation_timeout = config.live_gateway_operation_timeout,
            .live_gateway_turn_completion_timeout = config.live_gateway_turn_completion_timeout,
        },
        *suite);
}

} // namespace isla::server::evals
