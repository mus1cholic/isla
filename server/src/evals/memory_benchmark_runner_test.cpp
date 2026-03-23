#include "isla/server/evals/memory_benchmark_runner.hpp"

#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include <nlohmann/json.hpp>

namespace isla::server::evals {
namespace {

using nlohmann::json;
using nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// BuildMemoryBenchmarkReportJson tests
// ---------------------------------------------------------------------------

TEST(BuildMemoryBenchmarkReportJsonTest, EmptyReport) {
    const MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
    };

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    EXPECT_EQ(result["benchmark_name"], "test_bench");
    EXPECT_EQ(result["output_directory"], "out/test_bench");
    EXPECT_EQ(result["total_cases"], 0);
    EXPECT_EQ(result["passed_cases"], 0);
    EXPECT_EQ(result["failed_cases"], 0);
    EXPECT_TRUE(result["cases"].is_array());
    EXPECT_EQ(result["cases"].size(), 0U);
}

TEST(BuildMemoryBenchmarkReportJsonTest, CaseFieldsPreserved) {
    MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
        .total_cases = 1,
        .passed_cases = 1,
    };
    report.cases.push_back(MemoryBenchmarkCaseReport{
        .case_id = "q1",
        .passed = true,
        .final_reply = "The answer is blue.",
        .expected_answer = "blue",
        .artifact_path = "out/test_bench/artifacts/q1.json",
    });

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    ASSERT_EQ(result["cases"].size(), 1U);
    const ordered_json& case_json = result["cases"][0];
    EXPECT_EQ(case_json["case_id"], "q1");
    EXPECT_EQ(case_json["passed"], true);
    EXPECT_EQ(case_json["final_reply"], "The answer is blue.");
    EXPECT_EQ(case_json["expected_answer"], "blue");
    EXPECT_EQ(case_json["artifact_path"], "out/test_bench/artifacts/q1.json");
}

TEST(BuildMemoryBenchmarkReportJsonTest, MetadataPreservedWhenPresent) {
    MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
        .total_cases = 1,
    };
    report.cases.push_back(MemoryBenchmarkCaseReport{
        .case_id = "q1",
        .metadata = json{ { "question_type", "temporal-reasoning" }, { "difficulty", 3 } },
    });

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    ASSERT_EQ(result["cases"].size(), 1U);
    const ordered_json& case_json = result["cases"][0];
    ASSERT_TRUE(case_json.contains("metadata"));
    EXPECT_EQ(case_json["metadata"]["question_type"], "temporal-reasoning");
    EXPECT_EQ(case_json["metadata"]["difficulty"], 3);
}

TEST(BuildMemoryBenchmarkReportJsonTest, NullMetadataOmitted) {
    MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
        .total_cases = 1,
    };
    report.cases.push_back(MemoryBenchmarkCaseReport{
        .case_id = "q1",
    });

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    ASSERT_EQ(result["cases"].size(), 1U);
    EXPECT_FALSE(result["cases"][0].contains("metadata"));
}

TEST(BuildMemoryBenchmarkReportJsonTest, NullOptionalFieldsSerializedAsNull) {
    MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
        .total_cases = 1,
        .failed_cases = 1,
    };
    report.cases.push_back(MemoryBenchmarkCaseReport{
        .case_id = "q1",
        .passed = false,
        // final_reply, expected_answer, artifact_path all nullopt
    });

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    ASSERT_EQ(result["cases"].size(), 1U);
    const ordered_json& case_json = result["cases"][0];
    EXPECT_TRUE(case_json["final_reply"].is_null());
    EXPECT_TRUE(case_json["expected_answer"].is_null());
    EXPECT_TRUE(case_json["artifact_path"].is_null());
}

TEST(BuildMemoryBenchmarkReportJsonTest, MultipleCasesPreserved) {
    MemoryBenchmarkReport report{
        .benchmark_name = "test_bench",
        .output_directory = "out/test_bench",
        .total_cases = 3,
        .passed_cases = 2,
        .failed_cases = 1,
    };
    report.cases.push_back(MemoryBenchmarkCaseReport{ .case_id = "q1", .passed = true });
    report.cases.push_back(MemoryBenchmarkCaseReport{ .case_id = "q2", .passed = false });
    report.cases.push_back(MemoryBenchmarkCaseReport{ .case_id = "q3", .passed = true });

    const ordered_json result = BuildMemoryBenchmarkReportJson(report);
    EXPECT_EQ(result["total_cases"], 3);
    EXPECT_EQ(result["passed_cases"], 2);
    EXPECT_EQ(result["failed_cases"], 1);
    ASSERT_EQ(result["cases"].size(), 3U);
    EXPECT_EQ(result["cases"][0]["case_id"], "q1");
    EXPECT_EQ(result["cases"][1]["case_id"], "q2");
    EXPECT_EQ(result["cases"][2]["case_id"], "q3");
}

// ---------------------------------------------------------------------------
// RunMemoryBenchmark validation tests
// ---------------------------------------------------------------------------

TEST(RunMemoryBenchmarkTest, EmptyBenchmarkNameReturnsError) {
    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "";
    suite.cases.push_back(MemoryBenchmarkCase{});
    const absl::StatusOr<MemoryBenchmarkReport> result =
        RunMemoryBenchmark(MemoryBenchmarkRunConfig{}, suite);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
}

TEST(RunMemoryBenchmarkTest, EmptyCasesReturnsError) {
    const absl::StatusOr<MemoryBenchmarkReport> result = RunMemoryBenchmark(
        MemoryBenchmarkRunConfig{}, MemoryBenchmarkSuite{ .benchmark_name = "test" });
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
}

} // namespace
} // namespace isla::server::evals
