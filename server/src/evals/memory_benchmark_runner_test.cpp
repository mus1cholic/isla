#include "isla/server/evals/memory_benchmark_runner.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include <nlohmann/json.hpp>

#include "openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::test::FakeOpenAiResponsesClient;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::MessageRole;
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
// SanitizeCaseIdForFilename tests
// ---------------------------------------------------------------------------

TEST(SanitizeCaseIdForFilenameTest, AlphanumericPassedThrough) {
    EXPECT_EQ(SanitizeCaseIdForFilename("simple123"), "simple123");
}

TEST(SanitizeCaseIdForFilenameTest, HyphensPreserved) {
    EXPECT_EQ(SanitizeCaseIdForFilename("my-case-id"), "my-case-id");
}

TEST(SanitizeCaseIdForFilenameTest, SlashesReplacedWithUnderscore) {
    EXPECT_EQ(SanitizeCaseIdForFilename("path/to/case"), "path_to_case");
}

TEST(SanitizeCaseIdForFilenameTest, PathTraversalNeutralized) {
    EXPECT_EQ(SanitizeCaseIdForFilename("../../etc/passwd"), "etc_passwd");
}

TEST(SanitizeCaseIdForFilenameTest, BackslashesReplaced) {
    EXPECT_EQ(SanitizeCaseIdForFilename("path\\to\\case"), "path_to_case");
}

TEST(SanitizeCaseIdForFilenameTest, ConsecutiveSpecialCharsCollapsed) {
    EXPECT_EQ(SanitizeCaseIdForFilename("a///b"), "a_b");
}

TEST(SanitizeCaseIdForFilenameTest, SpacesAndSpecialCharsReplaced) {
    EXPECT_EQ(SanitizeCaseIdForFilename("hello world!@#"), "hello_world");
}

TEST(SanitizeCaseIdForFilenameTest, EmptyInputReturnsUnnamed) {
    EXPECT_EQ(SanitizeCaseIdForFilename(""), "unnamed");
}

TEST(SanitizeCaseIdForFilenameTest, AllSpecialCharsReturnsUnnamed) {
    EXPECT_EQ(SanitizeCaseIdForFilename("///..."), "unnamed");
}

TEST(SanitizeCaseIdForFilenameTest, LeadingTrailingSpecialCharsStripped) {
    EXPECT_EQ(SanitizeCaseIdForFilename("/case/"), "case");
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

TEST(RunMemoryBenchmarkTest, EmptyCaseIdReturnsError) {
    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "test_bench";

    EvalCase eval_case;
    eval_case.case_id = "";
    eval_case.session_id = "session_1";

    MemoryBenchmarkCase benchmark_case;
    benchmark_case.eval_case = std::move(eval_case);
    suite.cases.push_back(std::move(benchmark_case));

    const absl::StatusOr<MemoryBenchmarkReport> result =
        RunMemoryBenchmark(MemoryBenchmarkRunConfig{}, suite);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
    EXPECT_NE(std::string(result.status().message()).find("case 0"), std::string::npos);
    EXPECT_NE(std::string(result.status().message()).find("case_id"), std::string::npos);
}

TEST(RunMemoryBenchmarkTest, EmptySessionIdReturnsError) {
    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "test_bench";

    EvalCase eval_case;
    eval_case.case_id = "q1";
    eval_case.session_id = "";

    MemoryBenchmarkCase benchmark_case;
    benchmark_case.eval_case = std::move(eval_case);
    suite.cases.push_back(std::move(benchmark_case));

    const absl::StatusOr<MemoryBenchmarkReport> result =
        RunMemoryBenchmark(MemoryBenchmarkRunConfig{}, suite);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
    EXPECT_NE(std::string(result.status().message()).find("q1"), std::string::npos);
    EXPECT_NE(std::string(result.status().message()).find("session_id"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Integration test helpers
// ---------------------------------------------------------------------------

class ScopedOutputDirectory {
  public:
    ScopedOutputDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("memory_benchmark_runner_test_" +
                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))) {}

    ~ScopedOutputDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ScopedOutputDirectory(const ScopedOutputDirectory&) = delete;
    ScopedOutputDirectory& operator=(const ScopedOutputDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// Creates a FakeOpenAiResponsesClient with a StreamHandler that returns valid JSON for mid-term
// flush decider and compactor requests (identified by distinctive prompt substrings), and the
// configured user_reply for all other (user-facing) requests.
std::shared_ptr<FakeOpenAiResponsesClient> MakeMidTermAwareFakeClient(std::string user_reply) {
    return MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), /*full_text=*/"", /*response_id=*/"resp_test",
        /*validate_status=*/absl::OkStatus(),
        /*stream_handler=*/
        [reply = std::move(user_reply)](
            const isla::server::ai_gateway::OpenAiResponsesRequest& request,
            const isla::server::ai_gateway::OpenAiResponsesEventCallback& on_event)
            -> absl::Status {
            std::string text;
            if (request.system_prompt.find("should_flush") != std::string::npos) {
                text =
                    R"({"should_flush":false,"item_id":null,"split_at":null,"reasoning":"test"})";
            } else if (request.system_prompt.find("tier2_summary") != std::string::npos) {
                text =
                    R"({"tier1_detail":"d","tier2_summary":"s","tier3_ref":"r","tier3_keywords":["k"],"salience":5})";
            } else {
                text = reply;
            }
            if (const absl::Status s = on_event(
                    isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent{ .text_delta = text });
                !s.ok()) {
                return s;
            }
            return on_event(isla::server::ai_gateway::OpenAiResponsesCompletedEvent{
                .response_id = "resp_fake" });
        });
}

MemoryBenchmarkSuite MakeSingleCaseSuite(std::string case_id = "test_q1",
                                         std::string expected_answer = "blue",
                                         nlohmann::json metadata = nullptr) {
    EvalCase eval_case;
    // benchmark_name intentionally left empty — the runner auto-populates it from
    // suite.benchmark_name.
    eval_case.case_id = case_id;
    eval_case.session_id = "session_1";
    eval_case.conversation = {
        EvalConversationMessage{
            .role = MessageRole::User,
            .text = "My favorite color is blue.",
        },
        EvalConversationMessage{
            .role = MessageRole::Assistant,
            .text = "Got it, your favorite color is blue!",
        },
    };
    eval_case.input = EvalInput{ .text = "What is my favorite color?" };
    eval_case.expected_answer = expected_answer;

    MemoryBenchmarkCase benchmark_case;
    benchmark_case.eval_case = std::move(eval_case);
    benchmark_case.metadata = std::move(metadata);

    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "test_bench";
    suite.cases.push_back(std::move(benchmark_case));
    return suite;
}

// ---------------------------------------------------------------------------
// RunMemoryBenchmark integration tests
// ---------------------------------------------------------------------------

TEST(RunMemoryBenchmarkTest, SingleCasePassesAndWritesArtifacts) {
    ScopedOutputDirectory output_directory;

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.openai_client = MakeMidTermAwareFakeClient("The answer is blue.");

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->benchmark_name, "test_bench");
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);

    const MemoryBenchmarkCaseReport& case_report = report->cases.front();
    EXPECT_EQ(case_report.case_id, "test_q1");
    EXPECT_TRUE(case_report.passed);
    EXPECT_TRUE(case_report.final_reply.has_value());
    EXPECT_EQ(case_report.expected_answer, "blue");
    EXPECT_TRUE(case_report.artifact_path.has_value());

    // Verify artifact and report files exist on disk.
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "report.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" / "test_q1.json"));
}

TEST(RunMemoryBenchmarkTest, MultipleCasesTracksPassedAndFailed) {
    ScopedOutputDirectory output_directory;

    // Build a suite with two cases.
    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "multi_bench";

    for (const std::string& case_id : { "case_a", "case_b" }) {
        EvalCase eval_case;
        eval_case.case_id = case_id;
        eval_case.session_id = "session_1";
        eval_case.conversation = {
            EvalConversationMessage{
                .role = MessageRole::User,
                .text = "Hello",
            },
            EvalConversationMessage{
                .role = MessageRole::Assistant,
                .text = "Hi there!",
            },
        };
        eval_case.input = EvalInput{ .text = "Tell me something." };
        eval_case.expected_answer = "something";

        MemoryBenchmarkCase benchmark_case;
        benchmark_case.eval_case = std::move(eval_case);
        suite.cases.push_back(std::move(benchmark_case));
    }

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.openai_client = MakeMidTermAwareFakeClient("some answer");

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->benchmark_name, "multi_bench");
    EXPECT_EQ(report->total_cases, 2U);
    EXPECT_EQ(report->passed_cases, 2U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 2U);
    EXPECT_EQ(report->cases[0].case_id, "case_a");
    EXPECT_EQ(report->cases[1].case_id, "case_b");

    // Both artifact files should exist.
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" / "case_a.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" / "case_b.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "report.json"));
}

TEST(RunMemoryBenchmarkTest, MetadataPreservedThroughRunLoop) {
    ScopedOutputDirectory output_directory;

    const json metadata = json{ { "question_type", "temporal-reasoning" }, { "difficulty", 3 } };
    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite("meta_q1", "expected", metadata);

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.openai_client = MakeMidTermAwareFakeClient("answer text");

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    ASSERT_EQ(report->cases.size(), 1U);
    const MemoryBenchmarkCaseReport& case_report = report->cases.front();
    EXPECT_FALSE(case_report.metadata.is_null());
    EXPECT_EQ(case_report.metadata["question_type"], "temporal-reasoning");
    EXPECT_EQ(case_report.metadata["difficulty"], 3);
}

TEST(RunMemoryBenchmarkTest, ValidateFailureReturnsError) {
    auto failing_client = MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), /*full_text=*/"", /*response_id=*/"resp_test",
        /*validate_status=*/absl::InternalError("connection refused"));

    MemoryBenchmarkRunConfig config;
    config.openai_client = failing_client;

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(absl::IsInternal(report.status()));
}

TEST(RunMemoryBenchmarkTest, EmptyReplyMarkedAsFailed) {
    ScopedOutputDirectory output_directory;

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    // Empty user reply — mid-term requests still get valid JSON, but the user-facing turn
    // produces an empty final_reply.
    config.openai_client = MakeMidTermAwareFakeClient("");

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 0U);
    EXPECT_EQ(report->failed_cases, 1U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_FALSE(report->cases.front().passed);
}

TEST(RunMemoryBenchmarkTest, ArtifactFileContainsCaseAndExpectedAnswer) {
    ScopedOutputDirectory output_directory;

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.openai_client = MakeMidTermAwareFakeClient("The color is blue.");

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    // Read and parse the artifact file.
    const std::filesystem::path artifact_path =
        output_directory.path() / "artifacts" / "test_q1.json";
    ASSERT_TRUE(std::filesystem::exists(artifact_path));

    std::ifstream artifact_file(artifact_path);
    ASSERT_TRUE(artifact_file.is_open());
    const json artifact = json::parse(artifact_file);

    EXPECT_TRUE(artifact.contains("case"));
    EXPECT_EQ(artifact["case"]["case_id"], "test_q1");
    EXPECT_EQ(artifact["expected_answer"], "blue");
    EXPECT_TRUE(artifact.contains("artifacts"));
    EXPECT_TRUE(artifact.contains("final_reply"));
}

} // namespace
} // namespace isla::server::evals
