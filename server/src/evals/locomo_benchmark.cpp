#include "isla/server/evals/locomo_benchmark.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
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
using isla::server::memory::MessageRole;
using isla::server::memory::Timestamp;
using nlohmann::json;

constexpr std::string_view kBenchmarkName = "locomo";

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string CategoryLabel(int category) {
    switch (category) {
    case 1:
        return "single_hop";
    case 2:
        return "multi_hop";
    case 3:
        return "temporal";
    case 4:
        return "open_domain";
    case 5:
        return "adversarial";
    default:
        return absl::StrCat("unknown_", category);
    }
}

absl::StatusOr<int> GetRequiredIntegerField(const json& object, std::string_view field_name,
                                            std::string_view context) {
    const absl::StatusOr<const json*> field = GetRequiredField(object, field_name, context);
    if (!field.ok()) {
        return field.status();
    }
    if (!(*field)->is_number_integer()) {
        return invalid_argument(
            absl::StrCat(context, " field '", field_name, "' must be an integer"));
    }
    return (*field)->get<int>();
}

absl::StatusOr<std::optional<std::string>>
GetOptionalNormalizedStringOrIntegerField(const json& object, std::string_view field_name,
                                          std::string_view context) {
    const auto it = object.find(std::string(field_name));
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    const absl::StatusOr<std::string> normalized =
        NormalizeStringOrIntegerValue(*it, absl::StrCat(context, " field '", field_name, "'"));
    if (!normalized.ok()) {
        return normalized.status();
    }
    return *normalized;
}

absl::StatusOr<std::optional<json>> GetOptionalStringOrStringArrayField(const json& object,
                                                                        std::string_view field_name,
                                                                        std::string_view context) {
    const auto it = object.find(std::string(field_name));
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (it->is_string()) {
        return json(*it);
    }
    if (it->is_array()) {
        for (std::size_t index = 0; index < it->size(); ++index) {
            if (!(*it)[index].is_string()) {
                return invalid_argument(
                    absl::StrCat(context, " field '", field_name, "' must contain only strings"));
            }
        }
        return json(*it);
    }
    return invalid_argument(
        absl::StrCat(context, " field '", field_name, "' must be a string or string array"));
}

struct SessionTurns {
    int session_number = 0;
    Timestamp session_time;
    std::vector<EvalConversationMessage> messages;
    json turn_locations = json::array();
};

absl::StatusOr<std::vector<int>> ExtractSessionNumbers(const json& conversation,
                                                       std::string_view context) {
    std::vector<int> session_numbers;
    for (const auto& [key, value] : conversation.items()) {
        (void)value;
        if (!key.starts_with("session_") || key.ends_with("_date_time")) {
            continue;
        }
        const std::string_view prefix = "session_";
        const std::string_view suffix(key.data() + prefix.size(), key.size() - prefix.size());
        if (suffix.empty()) {
            continue;
        }
        int session_number = 0;
        const char* const begin = suffix.data();
        const char* const end = suffix.data() + suffix.size();
        const std::from_chars_result parse_result = std::from_chars(begin, end, session_number);
        if (parse_result.ec != std::errc() || parse_result.ptr != end) {
            continue;
        }
        session_numbers.push_back(session_number);
    }

    std::sort(session_numbers.begin(), session_numbers.end());
    session_numbers.erase(std::unique(session_numbers.begin(), session_numbers.end()),
                          session_numbers.end());
    if (session_numbers.empty()) {
        return invalid_argument(absl::StrCat(context, " must include at least one session_N key"));
    }
    return session_numbers;
}

absl::StatusOr<SessionTurns> ParseSessionTurns(const json& conversation, int session_number,
                                               std::string_view speaker_a,
                                               std::string_view speaker_b,
                                               std::size_t* flattened_turn_index,
                                               std::string_view context) {
    const std::string session_key = absl::StrCat("session_", session_number);
    const std::string date_key = absl::StrCat(session_key, "_date_time");

    const absl::StatusOr<const json*> session_field =
        GetRequiredField(conversation, session_key, context);
    if (!session_field.ok()) {
        return session_field.status();
    }
    if (!(**session_field).is_array()) {
        return invalid_argument(
            absl::StrCat(context, " field '", session_key, "' must be an array"));
    }

    const absl::StatusOr<std::string> session_time_text =
        GetRequiredStringField(conversation, date_key, context);
    if (!session_time_text.ok()) {
        return session_time_text.status();
    }
    const absl::StatusOr<Timestamp> session_time = ParseBenchmarkTimestamp(
        *session_time_text, absl::StrCat(context, " field '", date_key, "'"));
    if (!session_time.ok()) {
        return session_time.status();
    }

    SessionTurns parsed{
        .session_number = session_number,
        .session_time = *session_time,
    };
    parsed.messages.reserve((**session_field).size());

    for (std::size_t turn_index = 0; turn_index < (**session_field).size(); ++turn_index) {
        const json& turn = (**session_field)[turn_index];
        if (!turn.is_object()) {
            return invalid_argument(absl::StrCat(context, " ", session_key, "[", turn_index,
                                                 "] must be a JSON object"));
        }
        const std::string turn_context =
            absl::StrCat(context, " ", session_key, "[", turn_index, "]");

        const absl::StatusOr<std::string> speaker =
            GetRequiredStringField(turn, "speaker", turn_context);
        if (!speaker.ok()) {
            return speaker.status();
        }
        const absl::StatusOr<std::string> turn_text =
            GetRequiredStringField(turn, "text", turn_context);
        if (!turn_text.ok()) {
            return turn_text.status();
        }
        const absl::StatusOr<std::string> dia_id =
            GetRequiredStringField(turn, "dia_id", turn_context);
        if (!dia_id.ok()) {
            return dia_id.status();
        }

        MessageRole role = MessageRole::User;
        if (*speaker == speaker_a) {
            role = MessageRole::User;
        } else if (*speaker == speaker_b) {
            role = MessageRole::Assistant;
        } else {
            return invalid_argument(absl::StrCat(turn_context, " speaker '", *speaker,
                                                 "' must match speaker_a or speaker_b"));
        }

        std::string normalized_text = *turn_text;
        const auto caption_it = turn.find("blip_caption");
        if (caption_it != turn.end() && !caption_it->is_null()) {
            if (!caption_it->is_string()) {
                return invalid_argument(absl::StrCat(
                    turn_context, " field 'blip_caption' must be a string when present"));
            }
            if (!normalized_text.empty()) {
                normalized_text.append("\n");
            }
            normalized_text.append("[Image caption: ");
            normalized_text.append(caption_it->get<std::string>());
            normalized_text.push_back(']');
        }

        parsed.messages.push_back(EvalConversationMessage{
            .role = role,
            .text = std::move(normalized_text),
            .create_time = *session_time,
        });

        json location{
            { "dia_id", *dia_id },
            { "session_number", session_number },
            { "turn_index", turn_index },
            { "flattened_turn_index", *flattened_turn_index },
            { "speaker", *speaker },
            { "role", role == MessageRole::User ? "user" : "assistant" },
        };
        const absl::StatusOr<std::optional<json>> img_url =
            GetOptionalStringOrStringArrayField(turn, "img_url", turn_context);
        if (!img_url.ok()) {
            return img_url.status();
        }
        if (img_url->has_value()) {
            location["img_url"] = **img_url;
        }
        if (caption_it != turn.end() && !caption_it->is_null()) {
            location["blip_caption"] = caption_it->get<std::string>();
        }
        const auto query_it = turn.find("query");
        if (query_it != turn.end() && !query_it->is_null()) {
            if (!query_it->is_string()) {
                return invalid_argument(
                    absl::StrCat(turn_context, " field 'query' must be a string when present"));
            }
            location["query"] = query_it->get<std::string>();
        }
        parsed.turn_locations.push_back(std::move(location));
        ++(*flattened_turn_index);
    }

    return parsed;
}

absl::StatusOr<std::vector<MemoryBenchmarkCase>> ParseConversationCases(const json& entry,
                                                                        std::size_t entry_index) {
    if (!entry.is_object()) {
        return invalid_argument(
            absl::StrCat("LoCoMo entry ", entry_index, " must be a JSON object"));
    }

    const std::string entry_context = absl::StrCat("LoCoMo entry ", entry_index);
    const absl::StatusOr<std::string> sample_id =
        GetRequiredStringField(entry, "sample_id", entry_context);
    if (!sample_id.ok()) {
        return sample_id.status();
    }
    const std::string case_context = absl::StrCat("LoCoMo entry ", *sample_id);

    const absl::StatusOr<const json*> conversation_field =
        GetRequiredField(entry, "conversation", case_context);
    if (!conversation_field.ok()) {
        return conversation_field.status();
    }
    if (!(**conversation_field).is_object()) {
        return invalid_argument(
            absl::StrCat(case_context, " field 'conversation' must be an object"));
    }
    const json& conversation_object = **conversation_field;

    const absl::StatusOr<std::string> speaker_a =
        GetRequiredStringField(conversation_object, "speaker_a", case_context);
    if (!speaker_a.ok()) {
        return speaker_a.status();
    }
    const absl::StatusOr<std::string> speaker_b =
        GetRequiredStringField(conversation_object, "speaker_b", case_context);
    if (!speaker_b.ok()) {
        return speaker_b.status();
    }

    const absl::StatusOr<std::vector<int>> session_numbers =
        ExtractSessionNumbers(conversation_object, absl::StrCat(case_context, " conversation"));
    if (!session_numbers.ok()) {
        return session_numbers.status();
    }

    std::vector<EvalConversationMessage> conversation;
    json turn_locations = json::array();
    json session_dates = json::array();
    std::size_t flattened_turn_index = 0U;
    std::optional<Timestamp> session_start_time;
    std::optional<Timestamp> evaluation_reference_time;

    for (const int session_number : *session_numbers) {
        absl::StatusOr<SessionTurns> session =
            ParseSessionTurns(conversation_object, session_number, *speaker_a, *speaker_b,
                              &flattened_turn_index, case_context);
        if (!session.ok()) {
            return session.status();
        }

        if (!session_start_time.has_value() || session->session_time < *session_start_time) {
            session_start_time = session->session_time;
        }
        if (!evaluation_reference_time.has_value() ||
            session->session_time > *evaluation_reference_time) {
            evaluation_reference_time = session->session_time;
        }

        session_dates.push_back(json{
            { "session_number", session_number },
            { "timestamp", FormatTimestamp(session->session_time) },
        });

        conversation.insert(conversation.end(), session->messages.begin(), session->messages.end());
        for (json& location : session->turn_locations) {
            turn_locations.push_back(std::move(location));
        }
    }

    absl::flat_hash_map<std::string_view, const json*> location_by_dia_id;
    location_by_dia_id.reserve(turn_locations.size());
    for (const json& location : turn_locations) {
        const auto dia_id_it = location.find("dia_id");
        if (dia_id_it == location.end() || !dia_id_it->is_string()) {
            return invalid_argument(
                absl::StrCat(case_context, " turn_locations entry must include string dia_id"));
        }
        const std::string& dia_id = dia_id_it->get_ref<const std::string&>();
        location_by_dia_id.insert_or_assign(std::string_view(dia_id), &location);
    }

    const absl::StatusOr<const json*> qa_field = GetRequiredField(entry, "qa", case_context);
    if (!qa_field.ok()) {
        return qa_field.status();
    }
    if (!(**qa_field).is_array()) {
        return invalid_argument(absl::StrCat(case_context, " field 'qa' must be an array"));
    }

    std::vector<MemoryBenchmarkCase> benchmark_cases;
    benchmark_cases.reserve((**qa_field).size());

    for (std::size_t qa_index = 0; qa_index < (**qa_field).size(); ++qa_index) {
        const json& qa = (**qa_field)[qa_index];
        if (!qa.is_object()) {
            return invalid_argument(
                absl::StrCat(case_context, " qa[", qa_index, "] must be a JSON object"));
        }
        const std::string qa_context = absl::StrCat(case_context, " qa[", qa_index, "]");

        const absl::StatusOr<std::string> question =
            GetRequiredStringField(qa, "question", qa_context);
        if (!question.ok()) {
            return question.status();
        }
        const absl::StatusOr<std::vector<std::string>> evidence =
            GetRequiredStringArrayField(qa, "evidence", qa_context);
        if (!evidence.ok()) {
            return evidence.status();
        }
        const absl::StatusOr<int> category = GetRequiredIntegerField(qa, "category", qa_context);
        if (!category.ok()) {
            return category.status();
        }
        const absl::StatusOr<std::optional<std::string>> expected_answer =
            GetOptionalNormalizedStringOrIntegerField(qa, "answer", qa_context);
        if (!expected_answer.ok()) {
            return expected_answer.status();
        }
        const absl::StatusOr<std::optional<std::string>> adversarial_answer =
            GetOptionalNormalizedStringOrIntegerField(qa, "adversarial_answer", qa_context);
        if (!adversarial_answer.ok()) {
            return adversarial_answer.status();
        }

        const std::string case_id = absl::StrCat(*sample_id, "_q", qa_index + 1U);
        const absl::StatusOr<EvalCase> eval_case =
            BuildEvalCaseFromBenchmarkTimeline(EvalBenchmarkTimelineCase{
                .benchmark_name = std::string(kBenchmarkName),
                .case_id = case_id,
                .session_id = absl::StrCat(kBenchmarkName, "_", case_id),
                .session_start_time = session_start_time,
                .evaluation_reference_time = evaluation_reference_time,
                .conversation = conversation,
                .input =
                    EvalInput{
                        .text = *question,
                        .create_time = evaluation_reference_time,
                    },
                .expected_answer = *expected_answer,
            });
        if (!eval_case.ok()) {
            return eval_case.status();
        }

        json evidence_locations = json::array();
        for (const std::string& evidence_id : *evidence) {
            const auto location_it = location_by_dia_id.find(evidence_id);
            if (location_it == location_by_dia_id.end()) {
                evidence_locations.push_back(json{
                    { "dia_id", evidence_id },
                    { "missing_from_transcript", true },
                });
                continue;
            }
            evidence_locations.push_back(*location_it->second);
        }

        json metadata{
            { "sample_id", *sample_id },
            { "speaker_a", *speaker_a },
            { "speaker_b", *speaker_b },
            { "question_category", *category },
            { "question_category_label", CategoryLabel(*category) },
            { "question_index", qa_index + 1U },
            { "question_has_gold_answer", expected_answer->has_value() },
            { "evidence_dia_ids", *evidence },
            { "evidence_turn_locations", std::move(evidence_locations) },
            { "session_dates", session_dates },
            { "conversation_turn_count", conversation.size() },
        };
        if (adversarial_answer->has_value()) {
            metadata["adversarial_answer"] = **adversarial_answer;
        }

        benchmark_cases.push_back(MemoryBenchmarkCase{
            .eval_case = *eval_case,
            .metadata = std::move(metadata),
        });
    }

    return benchmark_cases;
}

absl::Status ValidateLoadConfig(const LoCoMoBenchmarkLoadConfig& config) {
    if (const absl::Status path_status =
            ValidateBenchmarkDatasetPath(config.dataset_path, "LoCoMo");
        !path_status.ok()) {
        return path_status;
    }
    if (const absl::Status sample_rate_status =
            ValidateBenchmarkSampleRate(config.sample_rate, "LoCoMo");
        !sample_rate_status.ok()) {
        return sample_rate_status;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::size_t> CountBenchmarkCases(const json& dataset) {
    std::size_t total_case_count = 0U;
    for (std::size_t entry_index = 0; entry_index < dataset.size(); ++entry_index) {
        const json& entry = dataset[entry_index];
        if (!entry.is_object()) {
            return invalid_argument(
                absl::StrCat("LoCoMo entry ", entry_index, " must be a JSON object"));
        }

        const std::string entry_context = absl::StrCat("LoCoMo entry ", entry_index);
        const absl::StatusOr<const json*> qa_field = GetRequiredField(entry, "qa", entry_context);
        if (!qa_field.ok()) {
            return qa_field.status();
        }
        if (!(**qa_field).is_array()) {
            return invalid_argument(absl::StrCat(entry_context, " field 'qa' must be an array"));
        }
        total_case_count += (**qa_field).size();
    }
    return total_case_count;
}

} // namespace

absl::StatusOr<MemoryBenchmarkSuite> LoadLoCoMoBenchmarkSuite(LoCoMoBenchmarkLoadConfig config) {
    if (absl::Status status = ValidateLoadConfig(config); !status.ok()) {
        return status;
    }

    const absl::StatusOr<json> dataset = ReadJsonFile(config.dataset_path, "LoCoMo dataset JSON");
    if (!dataset.ok()) {
        return dataset.status();
    }
    if (!dataset->is_array()) {
        return invalid_argument("LoCoMo dataset root must be a JSON array");
    }

    MemoryBenchmarkSuite suite;
    suite.benchmark_name = std::string(kBenchmarkName);
    const absl::StatusOr<std::size_t> total_case_count = CountBenchmarkCases(*dataset);
    if (!total_case_count.ok()) {
        return total_case_count.status();
    }
    suite.cases.reserve(*total_case_count);

    for (std::size_t entry_index = 0; entry_index < dataset->size(); ++entry_index) {
        absl::StatusOr<std::vector<MemoryBenchmarkCase>> parsed_cases =
            ParseConversationCases((*dataset)[entry_index], entry_index);
        if (!parsed_cases.ok()) {
            return parsed_cases.status();
        }
        for (MemoryBenchmarkCase& benchmark_case : *parsed_cases) {
            suite.cases.push_back(std::move(benchmark_case));
        }
    }

    if (config.case_id_filter.has_value()) {
        FilterCasesById(&suite.cases, *config.case_id_filter,
                        [](const MemoryBenchmarkCase& benchmark_case) -> std::string_view {
                            return benchmark_case.eval_case.case_id;
                        });
        if (suite.cases.empty()) {
            return invalid_argument("requested case_id_filter did not match any LoCoMo case");
        }
        return suite;
    }

    ApplyDeterministicSampling(&suite.cases, config.sample_rate, config.random_seed);
    if (suite.cases.empty()) {
        return invalid_argument(
            "LoCoMo sampling removed all cases; raise sample_rate or use case_id_filter");
    }

    return suite;
}

absl::StatusOr<MemoryBenchmarkReport> RunLoCoMoBenchmark(LoCoMoBenchmarkRunConfig config) {
    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLoCoMoBenchmarkSuite(LoCoMoBenchmarkLoadConfig{
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
