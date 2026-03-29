#include "isla/server/evals/benchmark_adapter_utils.hpp"

#include <cctype>
#include <charconv>
#include <exception>
#include <fstream>
#include <optional>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include <nlohmann/json.hpp>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::evals {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

bool IsDateOnlyText(std::string_view text) {
    if (text.size() != 10U) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4U || index == 7U) {
            if (text[index] != '-') {
                return false;
            }
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

bool IsLongMemEvalTimestampText(std::string_view text) {
    if (text.size() != 22U) {
        return false;
    }

    const auto is_digit = [text](std::size_t index) {
        return std::isdigit(static_cast<unsigned char>(text[index])) != 0;
    };
    const auto is_alpha = [text](std::size_t index) {
        return std::isalpha(static_cast<unsigned char>(text[index])) != 0;
    };

    return
        // YYYY/MM/DD
        is_digit(0) && is_digit(1) && is_digit(2) && is_digit(3) && text[4] == '/' && is_digit(5) &&
        is_digit(6) && text[7] == '/' && is_digit(8) && is_digit(9) &&
        //  (Dow)
        text[10] == ' ' && text[11] == '(' && is_alpha(12) && is_alpha(13) && is_alpha(14) &&
        text[15] == ')' &&
        //  HH:MM
        text[16] == ' ' && is_digit(17) && is_digit(18) && text[19] == ':' && is_digit(20) &&
        is_digit(21);
}

std::optional<unsigned> ParseLoCoMoMonth(std::string_view month_text) {
    static constexpr std::pair<std::string_view, unsigned> kMonths[] = {
        { "January", 1U },   { "February", 2U }, { "March", 3U },     { "April", 4U },
        { "May", 5U },       { "June", 6U },     { "July", 7U },      { "August", 8U },
        { "September", 9U }, { "October", 10U }, { "November", 11U }, { "December", 12U },
    };
    for (const auto& [candidate, month] : kMonths) {
        if (candidate == month_text) {
            return month;
        }
    }
    return std::nullopt;
}

absl::StatusOr<int> ParseIntegerText(std::string_view text, std::string_view field_description,
                                     std::string_view error_suffix) {
    int value = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const std::from_chars_result parse_result = std::from_chars(begin, end, value);
    if (parse_result.ec != std::errc() || parse_result.ptr != end) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: ", error_suffix));
    }
    return value;
}

absl::StatusOr<std::string> NormalizeLoCoMoTimestamp(std::string_view text,
                                                     std::string_view field_description) {
    const std::size_t on_pos = text.find(" on ");
    if (on_pos == std::string_view::npos) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: missing ' on '"));
    }

    const std::string_view time_part = text.substr(0, on_pos);
    const std::string_view date_part = text.substr(on_pos + 4U);
    if (time_part.size() < 7U) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: invalid time"));
    }

    const std::size_t colon_pos = time_part.find(':');
    const std::size_t space_pos =
        time_part.find(' ', colon_pos == std::string_view::npos ? 0U : colon_pos);
    if (colon_pos == std::string_view::npos || space_pos == std::string_view::npos ||
        space_pos + 2U >= time_part.size()) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: invalid time"));
    }

    int hour = 0;
    int minute = 0;
    const absl::StatusOr<int> parsed_hour =
        ParseIntegerText(time_part.substr(0, colon_pos), field_description, "invalid time digits");
    if (!parsed_hour.ok()) {
        return parsed_hour.status();
    }
    const absl::StatusOr<int> parsed_minute =
        ParseIntegerText(time_part.substr(colon_pos + 1U, space_pos - colon_pos - 1U),
                         field_description, "invalid time digits");
    if (!parsed_minute.ok()) {
        return parsed_minute.status();
    }
    hour = *parsed_hour;
    minute = *parsed_minute;
    const std::string_view meridiem = time_part.substr(space_pos + 1U);
    if (meridiem != "am" && meridiem != "pm") {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: invalid meridiem"));
    }
    if (hour < 1 || hour > 12 || minute < 0 || minute > 59) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: time out of range"));
    }
    if (meridiem == "am") {
        if (hour == 12) {
            hour = 0;
        }
    } else if (hour != 12) {
        hour += 12;
    }

    const std::size_t first_space = date_part.find(' ');
    const std::size_t comma_pos = date_part.find(", ");
    if (first_space == std::string_view::npos || comma_pos == std::string_view::npos ||
        comma_pos <= first_space + 1U) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: invalid date"));
    }

    int day = 0;
    int year = 0;
    const absl::StatusOr<int> parsed_day = ParseIntegerText(
        date_part.substr(0, first_space), field_description, "invalid date digits");
    if (!parsed_day.ok()) {
        return parsed_day.status();
    }
    const absl::StatusOr<int> parsed_year = ParseIntegerText(
        date_part.substr(comma_pos + 2U), field_description, "invalid date digits");
    if (!parsed_year.ok()) {
        return parsed_year.status();
    }
    day = *parsed_day;
    year = *parsed_year;

    const std::string_view month_text =
        date_part.substr(first_space + 1U, comma_pos - first_space - 1U);
    const std::optional<unsigned> month = ParseLoCoMoMonth(month_text);
    if (!month.has_value()) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: invalid month"));
    }

    return absl::StrFormat("%d-%02u-%02dT%02d:%02d:00Z", year, *month, day, hour, minute);
}

} // namespace

absl::Status ValidateBenchmarkDatasetPath(const std::filesystem::path& path,
                                          std::string_view dataset_label) {
    if (path.empty()) {
        return invalid_argument(absl::StrCat(dataset_label, " dataset_path must not be empty"));
    }
    return absl::OkStatus();
}

absl::Status ValidateBenchmarkSampleRate(double sample_rate, std::string_view dataset_label) {
    if (sample_rate <= 0.0 || sample_rate > 1.0) {
        return invalid_argument(
            absl::StrCat(dataset_label, " sample_rate must be in the range (0, 1]"));
    }
    return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> ReadJsonFile(std::filesystem::path path,
                                            std::string_view file_description) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return absl::NotFoundError(
            absl::StrCat("failed to open ", file_description, ": ", path.string()));
    }

    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return absl::InternalError(
            absl::StrCat("failed to read ", file_description, ": ", path.string()));
    }

    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception& error) {
        return invalid_argument(
            absl::StrCat("failed to parse ", file_description, ": ", error.what()));
    }
}

absl::StatusOr<const nlohmann::json*> GetRequiredField(const nlohmann::json& object,
                                                       std::string_view field_name,
                                                       std::string_view context) {
    const auto it = object.find(std::string(field_name));
    if (it == object.end()) {
        return invalid_argument(absl::StrCat(context, " is missing field '", field_name, "'"));
    }
    return &(*it);
}

absl::StatusOr<std::string> GetRequiredStringField(const nlohmann::json& object,
                                                   std::string_view field_name,
                                                   std::string_view context) {
    const absl::StatusOr<const nlohmann::json*> field =
        GetRequiredField(object, field_name, context);
    if (!field.ok()) {
        return field.status();
    }
    if (!(*field)->is_string()) {
        return invalid_argument(
            absl::StrCat(context, " field '", field_name, "' must be a string"));
    }
    return (*field)->get<std::string>();
}

absl::StatusOr<std::vector<std::string>> GetRequiredStringArrayField(const nlohmann::json& object,
                                                                     std::string_view field_name,
                                                                     std::string_view context) {
    const absl::StatusOr<const nlohmann::json*> field =
        GetRequiredField(object, field_name, context);
    if (!field.ok()) {
        return field.status();
    }
    if (!(*field)->is_array()) {
        return invalid_argument(
            absl::StrCat(context, " field '", field_name, "' must be an array"));
    }

    std::vector<std::string> values;
    values.reserve((*field)->size());
    for (std::size_t index = 0; index < (*field)->size(); ++index) {
        if (!(**field)[index].is_string()) {
            return invalid_argument(
                absl::StrCat(context, " field '", field_name, "' must contain only strings"));
        }
        values.push_back((**field)[index].get<std::string>());
    }
    return values;
}

absl::StatusOr<std::string> NormalizeStringOrIntegerValue(const nlohmann::json& value,
                                                          std::string_view value_description) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    return invalid_argument(absl::StrCat(value_description, " must be a string or integer"));
}

absl::StatusOr<isla::server::memory::Timestamp>
ParseBenchmarkTimestamp(std::string_view text, std::string_view field_description) {
    try {
        std::string normalized;
        if (IsDateOnlyText(text)) {
            normalized = absl::StrCat(text, "T00:00:00Z");
        } else if (IsLongMemEvalTimestampText(text)) {
            normalized =
                absl::StrCat(text.substr(0, 4), "-", text.substr(5, 2), "-", text.substr(8, 2), "T",
                             text.substr(17, 2), ":", text.substr(20, 2), ":00Z");
        } else if (text.find(" on ") != std::string_view::npos) {
            const absl::StatusOr<std::string> locomo_timestamp =
                NormalizeLoCoMoTimestamp(text, field_description);
            if (!locomo_timestamp.ok()) {
                return locomo_timestamp.status();
            }
            normalized = *locomo_timestamp;
        } else {
            normalized = std::string(text);
        }
        return isla::server::memory::ParseTimestamp(normalized);
    } catch (const std::exception& error) {
        return invalid_argument(
            absl::StrCat(field_description, " is not a supported timestamp: ", error.what()));
    }
}

absl::StatusOr<isla::server::memory::MessageRole>
ParseBenchmarkConversationRole(std::string_view role_text, std::string_view field_description) {
    if (role_text == "user") {
        return isla::server::memory::MessageRole::User;
    }
    if (role_text == "assistant") {
        return isla::server::memory::MessageRole::Assistant;
    }
    return invalid_argument(
        absl::StrCat(field_description, " must be 'user' or 'assistant', got '", role_text, "'"));
}

} // namespace isla::server::evals
