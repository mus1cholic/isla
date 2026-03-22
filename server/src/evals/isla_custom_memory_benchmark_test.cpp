#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <string>

#include <gtest/gtest.h>

namespace isla::server::evals {
namespace {

TEST(IslaCustomMemoryBenchmarkTest, RejectsMissingLiveOpenAiConfig) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{});

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(report.status().message()).find("openai responses client is disabled"),
              std::string::npos);
}

TEST(IslaCustomMemoryBenchmarkTest, RejectsUnknownCaseIdBeforeProviderValidation) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .case_id_filter = std::string("missing_case"),
        });

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(report.status().message()).find("case_id_filter"), std::string::npos);
}

} // namespace
} // namespace isla::server::evals
