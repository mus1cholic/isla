#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::evals {
namespace {

std::filesystem::path MakeUniqueOutputDirectory() {
    static int counter = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("isla_custom_memory_eval_test_" + std::to_string(++counter));
    std::filesystem::remove_all(directory);
    return directory;
}

TEST(IslaCustomMemoryBenchmarkTest, RunsAllPhaseThreeCasesAndPersistsArtifacts) {
    const std::filesystem::path output_directory = MakeUniqueOutputDirectory();

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory,
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->benchmark_name, "isla_custom_memory");
    EXPECT_EQ(report->total_cases, 6U);
    EXPECT_EQ(report->passed_cases, 6U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 6U);

    const std::filesystem::path report_path = output_directory / "report.json";
    ASSERT_TRUE(std::filesystem::exists(report_path));
    std::ifstream report_stream(report_path);
    ASSERT_TRUE(report_stream.is_open());
    const nlohmann::json report_json = nlohmann::json::parse(report_stream);
    report_stream.close();
    EXPECT_EQ(report_json.at("passed_cases"), 6U);
    EXPECT_EQ(report_json.at("failed_cases"), 0U);

    for (const IslaCustomMemoryCaseReport& case_report : report->cases) {
        EXPECT_TRUE(case_report.passed) << case_report.case_id;
        ASSERT_TRUE(case_report.artifact_path.has_value()) << case_report.case_id;
        const std::filesystem::path artifact_path =
            output_directory / "artifacts" / (case_report.suite_id + ".json");
        EXPECT_TRUE(std::filesystem::exists(artifact_path)) << case_report.case_id;
        std::ifstream artifact_stream(artifact_path);
        ASSERT_TRUE(artifact_stream.is_open()) << case_report.case_id;
        const nlohmann::json artifact_json = nlohmann::json::parse(artifact_stream);
        artifact_stream.close();
        EXPECT_EQ(artifact_json.at("suite_id"), case_report.suite_id) << case_report.case_id;
        ASSERT_TRUE(artifact_json.contains("cases")) << case_report.case_id;
        ASSERT_TRUE(artifact_json.at("cases").is_array()) << case_report.case_id;
        const auto matching_case =
            std::find_if(artifact_json.at("cases").begin(), artifact_json.at("cases").end(),
                         [&case_report](const nlohmann::json& candidate) {
                             return candidate.contains("case_id") &&
                                    candidate.at("case_id") == case_report.case_id;
                         });
        ASSERT_NE(matching_case, artifact_json.at("cases").end()) << case_report.case_id;
        EXPECT_TRUE(matching_case->contains("history")) << case_report.case_id;
        EXPECT_TRUE(matching_case->contains("input")) << case_report.case_id;
        EXPECT_TRUE(matching_case->contains("expected_answer")) << case_report.case_id;
        EXPECT_FALSE(matching_case->contains("passed")) << case_report.case_id;
        EXPECT_FALSE(matching_case->contains("conversation")) << case_report.case_id;
        EXPECT_FALSE(matching_case->contains("artifacts")) << case_report.case_id;
    }

    std::filesystem::remove_all(output_directory);
}

TEST(IslaCustomMemoryBenchmarkTest, SupportsFilteringToOneCase) {
    const std::filesystem::path output_directory = MakeUniqueOutputDirectory();

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory,
            .case_id_filter = std::string("expandable_exact_detail"),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_EQ(report->cases[0].case_id, "expandable_exact_detail");
    EXPECT_EQ(report->cases[0].suite_id, "expandable_exact_detail");
    EXPECT_TRUE(report->cases[0].passed);

    std::filesystem::remove_all(output_directory);
}

} // namespace
} // namespace isla::server::evals
