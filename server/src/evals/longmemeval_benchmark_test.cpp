#include "isla/server/evals/longmemeval_benchmark.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::test::FakeOpenAiResponsesClient;
using isla::server::ai_gateway::test::MakeFakeOpenAiResponsesClient;
using isla::server::memory::MessageRole;
using isla::server::memory::ParseTimestamp;
using isla::server::memory::Timestamp;
using nlohmann::json;
using namespace std::chrono_literals;

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

    ScopedTempPath(const ScopedTempPath&) = delete;
    ScopedTempPath& operator=(const ScopedTempPath&) = delete;

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
        Stop();
    }

    [[nodiscard]] absl::Status Start() {
        return server_.Start();
    }

    void Stop() {
        if (stopped_) {
            return;
        }
        server_.Stop();
        stopped_ = true;
    }

    [[nodiscard]] std::uint16_t port() const {
        return server_.bound_port();
    }

  private:
    GatewayStubResponder responder_;
    GatewayServer server_;
    bool stopped_ = false;
};

absl::Status WriteDatasetFile(const std::filesystem::path& path, const json& payload) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return absl::InternalError("failed to create dataset directory");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return absl::InternalError("failed to open dataset file");
    }
    output << payload.dump(2);
    if (!output.good()) {
        return absl::InternalError("failed to write dataset file");
    }
    return absl::OkStatus();
}

json MakeTurn(std::string role, std::string content,
              std::optional<bool> has_answer = std::nullopt) {
    json turn{
        { "role", std::move(role) },
        { "content", std::move(content) },
    };
    if (has_answer.has_value()) {
        turn["has_answer"] = *has_answer;
    }
    return turn;
}

json MakeCase(std::string question_id, json answer, json haystack_dates, json haystack_sessions,
              json haystack_session_ids, std::string question_type = "single-session-user",
              std::string question = "What is the answer?",
              std::string question_date = "2026-03-20",
              json answer_session_ids = json::array({ "session_b" })) {
    return json{
        { "question_id", std::move(question_id) },
        { "question_type", std::move(question_type) },
        { "question", std::move(question) },
        { "answer", std::move(answer) },
        { "question_date", std::move(question_date) },
        { "haystack_session_ids", std::move(haystack_session_ids) },
        { "haystack_dates", std::move(haystack_dates) },
        { "haystack_sessions", std::move(haystack_sessions) },
        { "answer_session_ids", std::move(answer_session_ids) },
    };
}

std::shared_ptr<FakeOpenAiResponsesClient> MakeMidTermAwareFakeClient(std::string user_reply) {
    return MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), /*full_text=*/"", /*response_id=*/"resp_test",
        /*validate_status=*/absl::OkStatus(),
        /*stream_handler=*/
        [reply =
             std::move(user_reply)](const OpenAiResponsesRequest& request,
                                    const OpenAiResponsesEventCallback& on_event) -> absl::Status {
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

TEST(LongMemEvalBenchmarkTest, LoadsAndNormalizesDateOnlyDatasetFields) {
    ScopedTempPath temp_dir("longmemeval_load_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    const json dataset = json::array({ MakeCase(
        "q_alpha", 7, json::array({ "2026-03-01", "2026-03-05" }),
        json::array(
            { json::array({ MakeTurn("user", "I like tea."), MakeTurn("assistant", "Noted.") }),
              json::array({ MakeTurn("user", "I switched to coffee.", true) }) }),
        json::array({ "session_a", "session_b" }), "knowledge-update",
        "What drink do I prefer now?", "2026-03-20") });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .sample_rate = 1.0,
            .random_seed = 42,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    EXPECT_EQ(suite->benchmark_name, "longmemeval_s");
    ASSERT_EQ(suite->cases.size(), 1U);

    const MemoryBenchmarkCase& benchmark_case = suite->cases.front();
    EXPECT_EQ(benchmark_case.eval_case.case_id, "q_alpha");
    EXPECT_EQ(benchmark_case.eval_case.session_id, "longmemeval_s_q_alpha");
    EXPECT_EQ(benchmark_case.eval_case.input.text, "What drink do I prefer now?");
    EXPECT_EQ(benchmark_case.eval_case.input.create_time, Ts("2026-03-20T00:00:00Z"));
    EXPECT_EQ(benchmark_case.eval_case.session_start_time, Ts("2026-03-01T00:00:00Z"));
    EXPECT_EQ(benchmark_case.eval_case.evaluation_reference_time, Ts("2026-03-20T00:00:00Z"));
    ASSERT_TRUE(benchmark_case.eval_case.expected_answer.has_value());
    EXPECT_EQ(*benchmark_case.eval_case.expected_answer, "7");

    ASSERT_EQ(benchmark_case.eval_case.conversation.size(), 3U);
    EXPECT_EQ(benchmark_case.eval_case.conversation[0].role, MessageRole::User);
    EXPECT_EQ(benchmark_case.eval_case.conversation[0].create_time, Ts("2026-03-01T00:00:00Z"));
    EXPECT_EQ(benchmark_case.eval_case.conversation[2].role, MessageRole::User);
    EXPECT_EQ(benchmark_case.eval_case.conversation[2].create_time, Ts("2026-03-05T00:00:00Z"));

    EXPECT_EQ(benchmark_case.metadata["question_type"], "knowledge-update");
    EXPECT_EQ(benchmark_case.metadata["answer_session_ids"][0], "session_b");
    EXPECT_EQ(benchmark_case.metadata["haystack_session_ids"][0], "session_a");
    EXPECT_EQ(benchmark_case.metadata["haystack_dates"][0], "2026-03-01T00:00:00Z");
    EXPECT_EQ(benchmark_case.metadata["question_date"], "2026-03-20T00:00:00Z");
    ASSERT_EQ(benchmark_case.metadata["has_answer_turn_locations"].size(), 1U);
    EXPECT_EQ(benchmark_case.metadata["has_answer_turn_locations"][0]["session_index"], 1);
    EXPECT_EQ(benchmark_case.metadata["has_answer_turn_locations"][0]["turn_index"], 0);
    EXPECT_EQ(benchmark_case.metadata["has_answer_turn_locations"][0]["flattened_turn_index"], 2);
}

TEST(LongMemEvalBenchmarkTest, CaseIdFilterSelectsRequestedCaseWithoutSamplingItAway) {
    ScopedTempPath temp_dir("longmemeval_filter_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    const json dataset =
        json::array({ MakeCase("q_keep", "blue", json::array({ "2026-03-01" }),
                               json::array({ json::array({ MakeTurn("user", "Blue.") }) }),
                               json::array({ "session_a" })),
                      MakeCase("q_skip", "green", json::array({ "2026-03-02" }),
                               json::array({ json::array({ MakeTurn("user", "Green.") }) }),
                               json::array({ "session_b" })) });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .case_id_filter = std::string("q_keep"),
            .sample_rate = 0.01,
            .random_seed = 123,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    ASSERT_EQ(suite->cases.size(), 1U);
    EXPECT_EQ(suite->cases.front().eval_case.case_id, "q_keep");
}

TEST(LongMemEvalBenchmarkTest, PreservesAssistantFirstTurnsAtSessionBoundaries) {
    ScopedTempPath temp_dir("longmemeval_assistant_first_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    const json dataset = json::array({ MakeCase(
        "q_assistant_first", "blue",
        json::array({ "2023/05/20 (Sat) 02:21", "2023/05/21 (Sun) 03:24" }),
        json::array({ json::array({ MakeTurn("user", "My favorite color is blue."),
                                    MakeTurn("assistant", "Noted.") }),
                      json::array({ MakeTurn("assistant", "Here is some unrelated reply."),
                                    MakeTurn("user", "Please remember it."),
                                    MakeTurn("assistant", "I will.") }) }),
        json::array({ "session_a", "session_b" }), "single-session-user",
        "What is my favorite color?", "2023/05/30 (Tue) 23:40") });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .sample_rate = 1.0,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    ASSERT_EQ(suite->cases.size(), 1U);
    const MemoryBenchmarkCase& benchmark_case = suite->cases.front();
    ASSERT_EQ(benchmark_case.eval_case.conversation.size(), 5U);
    EXPECT_EQ(benchmark_case.eval_case.conversation[0].text, "My favorite color is blue.");
    EXPECT_EQ(benchmark_case.eval_case.conversation[1].text, "Noted.");
    EXPECT_EQ(benchmark_case.eval_case.conversation[2].text, "Here is some unrelated reply.");
    EXPECT_EQ(benchmark_case.eval_case.conversation[3].text, "Please remember it.");
    EXPECT_EQ(benchmark_case.eval_case.conversation[4].text, "I will.");
}

TEST(LongMemEvalBenchmarkTest, SamplingIsDeterministicForFixedSeed) {
    ScopedTempPath temp_dir("longmemeval_sample_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    json dataset = json::array();
    for (int i = 0; i < 10; ++i) {
        dataset.push_back(MakeCase(
            "q_" + std::to_string(i), "answer_" + std::to_string(i), json::array({ "2026-03-01" }),
            json::array({ json::array({ MakeTurn("user", "Fact " + std::to_string(i)) }) }),
            json::array({ "session_" + std::to_string(i) })));
    }
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const LongMemEvalBenchmarkLoadConfig load_config{
        .dataset_path = dataset_path,
        .sample_rate = 0.5,
        .random_seed = 999,
    };
    const absl::StatusOr<MemoryBenchmarkSuite> first = LoadLongMemEvalBenchmarkSuite(load_config);
    const absl::StatusOr<MemoryBenchmarkSuite> second = LoadLongMemEvalBenchmarkSuite(load_config);

    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(second.ok()) << second.status();
    ASSERT_EQ(first->cases.size(), second->cases.size());
    ASSERT_FALSE(first->cases.empty());
    for (std::size_t index = 0; index < first->cases.size(); ++index) {
        EXPECT_EQ(first->cases[index].eval_case.case_id, second->cases[index].eval_case.case_id);
    }
}

TEST(LongMemEvalBenchmarkTest, PreservesTranscriptThatStartsWithAssistant) {
    ScopedTempPath temp_dir("longmemeval_invalid_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    const json dataset = json::array(
        { MakeCase("q_bad", "bad", json::array({ "2026-03-01" }),
                   json::array({ json::array({ MakeTurn("assistant", "This starts wrong.") }) }),
                   json::array({ "session_a" })) });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .sample_rate = 1.0,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    ASSERT_EQ(suite->cases.size(), 1U);
    ASSERT_EQ(suite->cases.front().eval_case.conversation.size(), 1U);
    EXPECT_EQ(suite->cases.front().eval_case.conversation[0].role, MessageRole::Assistant);
    EXPECT_EQ(suite->cases.front().eval_case.conversation[0].text, "This starts wrong.");
}

TEST(LongMemEvalBenchmarkTest, PreservesEmptyTranscriptMessages) {
    ScopedTempPath temp_dir("longmemeval_empty_message_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";

    const json dataset = json::array({ MakeCase(
        "q_empty_message", "blue", json::array({ "2026-03-01" }),
        json::array({ json::array({ MakeTurn("user", "first"),
                                    MakeTurn("assistant", "reply"),
                                    MakeTurn("user", ""),
                                    MakeTurn("assistant", "Did you have a question?") }) }),
        json::array({ "session_a" }), "single-session-user", "What color?", "2026-03-20") });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLongMemEvalBenchmarkSuite(LongMemEvalBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .sample_rate = 1.0,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    ASSERT_EQ(suite->cases.size(), 1U);
    ASSERT_EQ(suite->cases.front().eval_case.conversation.size(), 4U);
    EXPECT_EQ(suite->cases.front().eval_case.conversation[2].role, MessageRole::User);
    EXPECT_EQ(suite->cases.front().eval_case.conversation[2].text, "");
}

TEST(LongMemEvalBenchmarkTest, RunsThroughGenericMemoryBenchmarkRunner) {
    ScopedTempPath temp_dir("longmemeval_run_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "longmemeval_s_cleaned.json";
    const std::filesystem::path output_dir = temp_dir.path() / "out";

    const json dataset = json::array(
        { MakeCase("q_run", "blue", json::array({ "2026-03-01", "2026-03-02" }),
                   json::array({ json::array({ MakeTurn("user", "My favorite color is blue."),
                                               MakeTurn("assistant", "Noted.") }),
                                 json::array({ MakeTurn("user", "Please remember that.") }) }),
                   json::array({ "session_a", "session_b" }), "single-session-user",
                   "What is my favorite color?", "2026-03-20") });
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("The answer is blue."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    LongMemEvalBenchmarkRunConfig run_config;
    run_config.dataset_path = dataset_path;
    run_config.output_directory = output_dir;
    run_config.sample_rate = 1.0;
    run_config.live_gateway_port = live_gateway.port();

    const absl::StatusOr<MemoryBenchmarkReport> report = RunLongMemEvalBenchmark(run_config);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->benchmark_name, "longmemeval_s");
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_EQ(report->cases.front().case_id, "q_run");
    EXPECT_TRUE(report->cases.front().passed);
    EXPECT_TRUE(std::filesystem::exists(output_dir / "report.json"));
    EXPECT_TRUE(std::filesystem::exists(output_dir / "artifacts" / "q_run.json"));
}

} // namespace
} // namespace isla::server::evals
