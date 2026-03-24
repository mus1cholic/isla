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

#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::test::FakeOpenAiResponsesClient;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::ConversationMessageWrite;
using isla::server::memory::Episode;
using isla::server::memory::MemorySessionRecord;
using isla::server::memory::MemoryStore;
using isla::server::memory::MemoryStoreSnapshot;
using isla::server::memory::MessageRole;
using nlohmann::json;
using nlohmann::ordered_json;
using namespace std::chrono_literals;

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

class ScopedLiveGatewayServer {
  public:
    explicit ScopedLiveGatewayServer(GatewayStubResponderConfig responder_config)
        : responder_(std::move(responder_config)), server_(
                                                       GatewayServerConfig{
                                                           .bind_host = "127.0.0.1",
                                                           .port = 0,
                                                           .listen_backlog = 4,
                                                       },
                                                       &responder_) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    ~ScopedLiveGatewayServer() {
        server_.Stop();
    }

    [[nodiscard]] absl::Status Start() {
        return server_.Start();
    }

    [[nodiscard]] std::uint16_t port() const {
        return server_.bound_port();
    }

  private:
    GatewayStubResponder responder_;
    GatewayServer server_;
};

class RecordingMemoryStore final : public MemoryStore {
  public:
    absl::Status UpsertSession(const MemorySessionRecord& record) override {
        session_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(const ConversationMessageWrite& write) override {
        message_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& write) override {
        stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& write) override {
        mid_term_episode_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& write) override {
        split_stub_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view /*session_id*/) const override {
        return std::vector<Episode>{};
    }

    absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view /*session_id*/,
                      std::string_view /*episode_id*/) const override {
        return std::nullopt;
    }

    absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view /*session_id*/) const override {
        return std::nullopt;
    }

    std::vector<MemorySessionRecord> session_records;
    std::vector<ConversationMessageWrite> message_writes;
    std::vector<isla::server::memory::EpisodeStubWrite> stub_writes;
    std::vector<isla::server::memory::SplitEpisodeStubWrite> split_stub_writes;
    std::vector<isla::server::memory::MidTermEpisodeWrite> mid_term_episode_writes;
};

class FakeBenchmarkLlmClient final : public isla::server::LlmClient {
  public:
    explicit FakeBenchmarkLlmClient(std::string user_reply) : user_reply_(std::move(user_reply)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const isla::server::LlmRequest& request,
                   const isla::server::LlmEventCallback& on_event) const override {
        std::string text;
        if (request.system_prompt.find("should_flush") != std::string::npos) {
            text = R"({"should_flush":false,"item_id":null,"split_at":null,"reasoning":"test"})";
        } else if (request.system_prompt.find("tier2_summary") != std::string::npos) {
            text =
                R"({"tier1_detail":"d","tier2_summary":"s","tier3_ref":"r","tier3_keywords":["k"],"salience":5})";
        } else {
            text = user_reply_;
        }
        if (!text.empty()) {
            const absl::Status delta_status = on_event(isla::server::LlmTextDeltaEvent{
                .text_delta = std::move(text),
            });
            if (!delta_status.ok()) {
                return delta_status;
            }
        }
        return on_event(isla::server::LlmCompletedEvent{
            .response_id = "resp_fake_llm",
        });
    }

  private:
    std::string user_reply_;
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
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("The answer is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

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

TEST(RunMemoryBenchmarkTest, SupportsInjectedGenericLlmClient) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .llm_client = std::make_shared<const FakeBenchmarkLlmClient>("The answer is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);
    ASSERT_TRUE(report->cases.front().final_reply.has_value());
    EXPECT_EQ(*report->cases.front().final_reply, "The answer is blue.");
}

TEST(RunMemoryBenchmarkTest, MultipleCasesTracksPassedAndFailed) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("some answer"),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

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
    config.live_gateway_port = live_gateway.port();

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
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("answer text"),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const json metadata = json{ { "question_type", "temporal-reasoning" }, { "difficulty", 3 } };
    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite("meta_q1", "expected", metadata);

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    ASSERT_EQ(report->cases.size(), 1U);
    const MemoryBenchmarkCaseReport& case_report = report->cases.front();
    EXPECT_FALSE(case_report.metadata.is_null());
    EXPECT_EQ(case_report.metadata["question_type"], "temporal-reasoning");
    EXPECT_EQ(case_report.metadata["difficulty"], 3);
}

TEST(RunMemoryBenchmarkTest, UnreachableLiveGatewayReturnsError) {
    MemoryBenchmarkRunConfig config;
    config.live_gateway_host = "127.0.0.1";
    config.live_gateway_port = 1;

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 0U);
    EXPECT_EQ(report->failed_cases, 1U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_FALSE(report->cases.front().passed);
    EXPECT_FALSE(report->cases.front().final_reply.has_value());
    EXPECT_FALSE(report->cases.front().artifact_path.has_value());
}

TEST(RunMemoryBenchmarkTest, PersistsConversationThroughGatewayMemoryStore) {
    ScopedOutputDirectory output_directory;
    auto store = std::make_shared<RecordingMemoryStore>();
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .memory_store = store,
        .openai_client = MakeMidTermAwareFakeClient("The answer is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();
    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_FALSE(store->session_records.empty());
    EXPECT_GE(store->message_writes.size(), 4U);
}

TEST(RunMemoryBenchmarkTest, GatewayTurnFailureReturnsError) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient(""),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();
    // Empty user reply — mid-term requests still get valid JSON, but the user-facing turn
    // produces an empty final_reply.

    const MemoryBenchmarkSuite suite = MakeSingleCaseSuite();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 0U);
    EXPECT_EQ(report->failed_cases, 1U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_FALSE(report->cases.front().passed);
    EXPECT_FALSE(report->cases.front().final_reply.has_value());
    EXPECT_FALSE(report->cases.front().artifact_path.has_value());
}

TEST(RunMemoryBenchmarkTest, ContinuesAfterOneCaseFails) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("The answer is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "mixed_bench";

    MemoryBenchmarkCase failing_case;
    failing_case.eval_case.case_id = "invalid_case";
    failing_case.eval_case.session_id = "session_invalid";
    failing_case.eval_case.input = EvalInput{ .text = "" };
    suite.cases.push_back(failing_case);

    MemoryBenchmarkCase passing_case = MakeSingleCaseSuite().cases.front();
    passing_case.eval_case.case_id = "valid_case";
    passing_case.eval_case.session_id = "session_valid";
    suite.cases.push_back(std::move(passing_case));

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 2U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 1U);
    ASSERT_EQ(report->cases.size(), 2U);
    EXPECT_EQ(report->cases[0].case_id, "invalid_case");
    EXPECT_FALSE(report->cases[0].passed);
    EXPECT_EQ(report->cases[1].case_id, "valid_case");
    EXPECT_TRUE(report->cases[1].passed);
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" / "valid_case.json"));
}

TEST(RunMemoryBenchmarkTest, ArtifactFileContainsCaseAndExpectedAnswer) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("The color is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();

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

TEST(RunMemoryBenchmarkTest, RaisedRenderedWorkingMemoryBudgetAllowsLargeBenchmarkCase) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("some answer"),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    MemoryBenchmarkSuite suite;
    suite.benchmark_name = "large_context_bench";

    EvalCase eval_case;
    eval_case.case_id = "large_case";
    eval_case.session_id = "session_1";
    const std::string large_message(24U * 1024U, 'x');
    eval_case.conversation = {
        EvalConversationMessage{
            .role = MessageRole::User,
            .text = large_message,
        },
        EvalConversationMessage{
            .role = MessageRole::Assistant,
            .text = large_message,
        },
        EvalConversationMessage{
            .role = MessageRole::User,
            .text = large_message,
        },
        EvalConversationMessage{
            .role = MessageRole::Assistant,
            .text = large_message,
        },
    };
    eval_case.input = EvalInput{ .text = "What did I tell you earlier?" };
    eval_case.expected_answer = "x";

    MemoryBenchmarkCase benchmark_case;
    benchmark_case.eval_case = std::move(eval_case);
    suite.cases.push_back(std::move(benchmark_case));

    MemoryBenchmarkRunConfig config;
    config.output_directory = output_directory.path();
    config.live_gateway_port = live_gateway.port();
    config.max_rendered_working_memory_context_bytes = 512U * 1024U;

    const absl::StatusOr<MemoryBenchmarkReport> report =
        RunMemoryBenchmark(std::move(config), suite);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);
    EXPECT_TRUE(report->cases.front().final_reply.has_value());
}

} // namespace
} // namespace isla::server::evals
