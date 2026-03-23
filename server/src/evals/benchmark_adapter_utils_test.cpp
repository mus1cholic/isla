#include "isla/server/evals/benchmark_adapter_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::memory::MessageRole;
using isla::server::memory::ParseTimestamp;
using isla::server::memory::Timestamp;
using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return ParseTimestamp(text);
}

class ScopedTempPath {
  public:
    explicit ScopedTempPath(std::string prefix)
        : path_(std::filesystem::temp_directory_path() /
                (std::move(prefix) +
                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))) {}

    ~ScopedTempPath() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

absl::Status WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return absl::InternalError("failed to create directory");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return absl::InternalError("failed to open file");
    }
    output << text;
    if (!output.good()) {
        return absl::InternalError("failed to write file");
    }
    return absl::OkStatus();
}

struct TestCase {
    std::string case_id;
};

TEST(BenchmarkAdapterUtilsTest, ValidateBenchmarkDatasetPathRejectsEmptyPath) {
    const absl::Status status = ValidateBenchmarkDatasetPath({}, "Test");
    EXPECT_TRUE(absl::IsInvalidArgument(status));
}

TEST(BenchmarkAdapterUtilsTest, ValidateBenchmarkSampleRateRejectsOutOfRangeValues) {
    EXPECT_TRUE(absl::IsInvalidArgument(ValidateBenchmarkSampleRate(0.0, "Test")));
    EXPECT_TRUE(absl::IsInvalidArgument(ValidateBenchmarkSampleRate(1.1, "Test")));
    EXPECT_TRUE(ValidateBenchmarkSampleRate(1.0, "Test").ok());
}

TEST(BenchmarkAdapterUtilsTest, ReadJsonFileParsesJsonPayload) {
    ScopedTempPath temp_dir("benchmark_adapter_utils_json_");
    const std::filesystem::path file_path = temp_dir.path() / "sample.json";
    ASSERT_TRUE(WriteTextFile(file_path, R"({"name":"alpha","count":3})").ok());

    const absl::StatusOr<json> parsed = ReadJsonFile(file_path, "test json");
    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ((*parsed)["name"], "alpha");
    EXPECT_EQ((*parsed)["count"], 3);
}

TEST(BenchmarkAdapterUtilsTest, ReadJsonFileRejectsInvalidJson) {
    ScopedTempPath temp_dir("benchmark_adapter_utils_bad_json_");
    const std::filesystem::path file_path = temp_dir.path() / "sample.json";
    ASSERT_TRUE(WriteTextFile(file_path, "{not json}").ok());

    const absl::StatusOr<json> parsed = ReadJsonFile(file_path, "test json");
    ASSERT_FALSE(parsed.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(parsed.status()));
}

TEST(BenchmarkAdapterUtilsTest, GetRequiredStringFieldRejectsWrongType) {
    const json payload{ { "name", 3 } };
    const absl::StatusOr<std::string> value =
        GetRequiredStringField(payload, "name", "test payload");
    ASSERT_FALSE(value.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(value.status()));
}

TEST(BenchmarkAdapterUtilsTest, GetRequiredStringArrayFieldParsesStrings) {
    const json payload{ { "items", json::array({ "a", "b" }) } };
    const absl::StatusOr<std::vector<std::string>> value =
        GetRequiredStringArrayField(payload, "items", "test payload");
    ASSERT_TRUE(value.ok()) << value.status();
    ASSERT_EQ(value->size(), 2U);
    EXPECT_EQ((*value)[0], "a");
    EXPECT_EQ((*value)[1], "b");
}

TEST(BenchmarkAdapterUtilsTest, NormalizeStringOrIntegerValueSupportsStringsAndIntegers) {
    EXPECT_EQ(NormalizeStringOrIntegerValue(json("blue"), "answer").value(), "blue");
    EXPECT_EQ(NormalizeStringOrIntegerValue(json(7), "answer").value(), "7");
}

TEST(BenchmarkAdapterUtilsTest, ParseBenchmarkTimestampSupportsDateOnlyAndIso8601) {
    const absl::StatusOr<Timestamp> date_only =
        ParseBenchmarkTimestamp("2026-03-20", "question_date");
    const absl::StatusOr<Timestamp> full_timestamp =
        ParseBenchmarkTimestamp("2026-03-20T09:30:00Z", "question_date");

    ASSERT_TRUE(date_only.ok()) << date_only.status();
    ASSERT_TRUE(full_timestamp.ok()) << full_timestamp.status();
    EXPECT_EQ(*date_only, Ts("2026-03-20T00:00:00Z"));
    EXPECT_EQ(*full_timestamp, Ts("2026-03-20T09:30:00Z"));
}

TEST(BenchmarkAdapterUtilsTest, ParseBenchmarkConversationRoleParsesKnownRoles) {
    EXPECT_EQ(ParseBenchmarkConversationRole("user", "role").value(), MessageRole::User);
    EXPECT_EQ(ParseBenchmarkConversationRole("assistant", "role").value(), MessageRole::Assistant);
    EXPECT_TRUE(absl::IsInvalidArgument(ParseBenchmarkConversationRole("system", "role").status()));
}

TEST(BenchmarkAdapterUtilsTest, FilterCasesByIdKeepsOnlyMatchingCase) {
    std::vector<TestCase> cases{ TestCase{ "a" }, TestCase{ "b" }, TestCase{ "c" } };
    FilterCasesById(&cases, "b", [](const TestCase& test_case) -> std::string_view {
        return test_case.case_id;
    });

    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0].case_id, "b");
}

TEST(BenchmarkAdapterUtilsTest, ApplyDeterministicSamplingIsStableForSameSeed) {
    std::vector<TestCase> first;
    std::vector<TestCase> second;
    for (int i = 0; i < 10; ++i) {
        first.push_back(TestCase{ std::to_string(i) });
        second.push_back(TestCase{ std::to_string(i) });
    }

    ApplyDeterministicSampling(&first, 0.5, 1234);
    ApplyDeterministicSampling(&second, 0.5, 1234);

    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].case_id, second[index].case_id);
    }
}

} // namespace
} // namespace isla::server::evals
