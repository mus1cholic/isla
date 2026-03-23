#include "isla/server/evals/benchmark_adapter_utils.hpp"

#include <exception>
#include <fstream>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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
        const std::string normalized =
            IsDateOnlyText(text) ? absl::StrCat(text, "T00:00:00Z") : std::string(text);
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
