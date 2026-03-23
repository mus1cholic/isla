#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::evals {

[[nodiscard]] absl::Status ValidateBenchmarkDatasetPath(const std::filesystem::path& path,
                                                        std::string_view dataset_label);
[[nodiscard]] absl::Status ValidateBenchmarkSampleRate(double sample_rate,
                                                       std::string_view dataset_label);

[[nodiscard]] absl::StatusOr<nlohmann::json> ReadJsonFile(std::filesystem::path path,
                                                          std::string_view file_description);
[[nodiscard]] absl::StatusOr<const nlohmann::json*> GetRequiredField(const nlohmann::json& object,
                                                                     std::string_view field_name,
                                                                     std::string_view context);
[[nodiscard]] absl::StatusOr<std::string> GetRequiredStringField(const nlohmann::json& object,
                                                                 std::string_view field_name,
                                                                 std::string_view context);
[[nodiscard]] absl::StatusOr<std::vector<std::string>>
GetRequiredStringArrayField(const nlohmann::json& object, std::string_view field_name,
                            std::string_view context);
[[nodiscard]] absl::StatusOr<std::string>
NormalizeStringOrIntegerValue(const nlohmann::json& value, std::string_view value_description);
[[nodiscard]] absl::StatusOr<isla::server::memory::Timestamp>
ParseBenchmarkTimestamp(std::string_view text, std::string_view field_description);
[[nodiscard]] absl::StatusOr<isla::server::memory::MessageRole>
ParseBenchmarkConversationRole(std::string_view role_text, std::string_view field_description);

template <typename CaseType, typename GetCaseIdFn>
void FilterCasesById(std::vector<CaseType>* cases, std::string_view case_id,
                     GetCaseIdFn&& get_case_id) {
    if (cases == nullptr) {
        return;
    }
    cases->erase(std::remove_if(cases->begin(), cases->end(),
                                [&case_id, &get_case_id](const CaseType& benchmark_case) {
                                    return get_case_id(benchmark_case) != case_id;
                                }),
                 cases->end());
}

template <typename CaseType>
void ApplyDeterministicSampling(std::vector<CaseType>* cases, double sample_rate,
                                std::uint32_t random_seed) {
    if (sample_rate >= 1.0 || cases == nullptr || cases->empty()) {
        return;
    }

    std::mt19937 random_engine(random_seed);
    std::bernoulli_distribution include_case(sample_rate);
    std::vector<CaseType> sampled_cases;
    const double estimated_sampled_count =
        std::ceil(static_cast<double>(cases->size()) * sample_rate);
    sampled_cases.reserve(static_cast<std::size_t>(estimated_sampled_count));
    for (CaseType& benchmark_case : *cases) {
        if (include_case(random_engine)) {
            sampled_cases.push_back(std::move(benchmark_case));
        }
    }
    *cases = std::move(sampled_cases);
}

} // namespace isla::server::evals
