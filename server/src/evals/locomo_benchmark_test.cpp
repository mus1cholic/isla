#include "isla/server/evals/locomo_benchmark.hpp"

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

json MakeTurn(std::string speaker, std::string dia_id, std::string text,
              std::optional<std::string> blip_caption = std::nullopt,
              std::optional<std::string> img_url = std::nullopt,
              std::optional<std::string> query = std::nullopt) {
    json turn{
        { "speaker", std::move(speaker) },
        { "dia_id", std::move(dia_id) },
        { "text", std::move(text) },
    };
    if (blip_caption.has_value()) {
        turn["blip_caption"] = *blip_caption;
    }
    if (img_url.has_value()) {
        turn["img_url"] = *img_url;
    }
    if (query.has_value()) {
        turn["query"] = *query;
    }
    return turn;
}

json MakeQa(std::string question, json evidence, int category,
            std::optional<json> answer = std::nullopt,
            std::optional<std::string> adversarial_answer = std::nullopt) {
    json qa{
        { "question", std::move(question) },
        { "evidence", std::move(evidence) },
        { "category", category },
    };
    if (answer.has_value()) {
        qa["answer"] = *answer;
    }
    if (adversarial_answer.has_value()) {
        qa["adversarial_answer"] = *adversarial_answer;
    }
    return qa;
}

json MakeConversationEntry() {
    return json{
        { "sample_id", "conv-26" },
        { "conversation",
          {
              { "speaker_a", "Caroline" },
              { "speaker_b", "Melanie" },
              { "session_1_date_time", "1:56 pm on 8 May, 2023" },
              { "session_1",
                json::array({ MakeTurn("Caroline", "D1:1", "Hey Mel! I went to yoga today."),
                              MakeTurn("Melanie", "D1:2", "Nice! I adopted a cat named Oscar."),
                              MakeTurn("Caroline", "D1:3", "Here is the pottery photo.",
                                       "a black and white bowl", "https://example.test/bowl.png",
                                       "pottery bowl") }) },
              { "session_2_date_time", "9:14 am on 25 May, 2023" },
              { "session_2",
                json::array({ MakeTurn("Melanie", "D2:1", "I still love Oscar."),
                              MakeTurn("Caroline", "D2:2",
                                       "I joined the LGBTQ support group on 7 May 2023.") }) },
              { "session_3_date_time", "4:00 pm on 30 May, 2023" },
          } },
        { "qa", json::array({ MakeQa("When did Caroline join the LGBTQ support group?",
                                     json::array({ "D2:2" }), 3, json("7 May 2023")),
                              MakeQa("Is Oscar Caroline's pet?", json::array({ "D1:2" }), 5,
                                     std::nullopt, "Yes") }) },
        { "event_summary", json::array() },
        { "observation", json::array() },
        { "session_summary", json::array() },
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

TEST(LoCoMoBenchmarkTest, LoadsAndExpandsConversationIntoPerQuestionCases) {
    ScopedTempPath temp_dir("locomo_load_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "locomo10.json";
    ASSERT_TRUE(WriteDatasetFile(dataset_path, json::array({ MakeConversationEntry() })).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLoCoMoBenchmarkSuite(LoCoMoBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .sample_rate = 1.0,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    EXPECT_EQ(suite->benchmark_name, "locomo");
    ASSERT_EQ(suite->cases.size(), 2U);

    const MemoryBenchmarkCase& recall_case = suite->cases[0];
    EXPECT_EQ(recall_case.eval_case.case_id, "conv-26_q1");
    EXPECT_EQ(recall_case.eval_case.session_id, "locomo_conv-26_q1");
    EXPECT_EQ(recall_case.eval_case.session_start_time, Ts("2023-05-08T13:56:00Z"));
    EXPECT_EQ(recall_case.eval_case.evaluation_reference_time, Ts("2023-05-25T09:14:00Z"));
    EXPECT_EQ(recall_case.eval_case.input.text, "When did Caroline join the LGBTQ support group?");
    EXPECT_EQ(recall_case.eval_case.input.create_time, Ts("2023-05-25T09:14:00Z"));
    ASSERT_TRUE(recall_case.eval_case.expected_answer.has_value());
    EXPECT_EQ(*recall_case.eval_case.expected_answer, "7 May 2023");

    ASSERT_EQ(recall_case.eval_case.conversation.size(), 5U);
    EXPECT_EQ(recall_case.eval_case.conversation[0].role, MessageRole::User);
    EXPECT_EQ(recall_case.eval_case.conversation[1].role, MessageRole::Assistant);
    EXPECT_EQ(recall_case.eval_case.conversation[2].text,
              "Here is the pottery photo.\n[Image caption: a black and white bowl]");
    EXPECT_EQ(recall_case.eval_case.conversation[2].create_time, Ts("2023-05-08T13:56:00Z"));
    EXPECT_EQ(recall_case.eval_case.conversation[3].role, MessageRole::Assistant);
    EXPECT_EQ(recall_case.eval_case.conversation[4].role, MessageRole::User);
    EXPECT_EQ(recall_case.metadata["sample_id"], "conv-26");
    EXPECT_EQ(recall_case.metadata["speaker_a"], "Caroline");
    EXPECT_EQ(recall_case.metadata["speaker_b"], "Melanie");
    EXPECT_EQ(recall_case.metadata["question_category"], 3);
    EXPECT_EQ(recall_case.metadata["question_category_label"], "temporal");
    EXPECT_EQ(recall_case.metadata["question_has_gold_answer"], true);
    EXPECT_EQ(recall_case.metadata["evidence_dia_ids"][0], "D2:2");
    ASSERT_EQ(recall_case.metadata["evidence_turn_locations"].size(), 1U);
    EXPECT_EQ(recall_case.metadata["evidence_turn_locations"][0]["flattened_turn_index"], 4);

    const MemoryBenchmarkCase& adversarial_case = suite->cases[1];
    EXPECT_EQ(adversarial_case.eval_case.case_id, "conv-26_q2");
    EXPECT_FALSE(adversarial_case.eval_case.expected_answer.has_value());
    EXPECT_EQ(adversarial_case.metadata["question_category"], 5);
    EXPECT_EQ(adversarial_case.metadata["question_category_label"], "adversarial");
    EXPECT_EQ(adversarial_case.metadata["question_has_gold_answer"], false);
    EXPECT_EQ(adversarial_case.metadata["adversarial_answer"], "Yes");
    ASSERT_EQ(adversarial_case.metadata["evidence_turn_locations"].size(), 1U);
    EXPECT_EQ(adversarial_case.metadata["evidence_turn_locations"][0]["dia_id"], "D1:2");
}

TEST(LoCoMoBenchmarkTest, CaseIdFilterSelectsRequestedCaseWithoutSamplingItAway) {
    ScopedTempPath temp_dir("locomo_filter_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "locomo10.json";
    ASSERT_TRUE(WriteDatasetFile(dataset_path, json::array({ MakeConversationEntry() })).ok());

    const absl::StatusOr<MemoryBenchmarkSuite> suite =
        LoadLoCoMoBenchmarkSuite(LoCoMoBenchmarkLoadConfig{
            .dataset_path = dataset_path,
            .case_id_filter = std::string("conv-26_q2"),
            .sample_rate = 0.01,
            .random_seed = 123,
        });

    ASSERT_TRUE(suite.ok()) << suite.status();
    ASSERT_EQ(suite->cases.size(), 1U);
    EXPECT_EQ(suite->cases.front().eval_case.case_id, "conv-26_q2");
}

TEST(LoCoMoBenchmarkTest, SamplingIsDeterministicForFixedSeed) {
    ScopedTempPath temp_dir("locomo_sample_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "locomo10.json";
    json dataset = json::array();
    for (int i = 0; i < 3; ++i) {
        json entry = MakeConversationEntry();
        entry["sample_id"] = "conv-" + std::to_string(30 + i);
        entry["qa"] = json::array({ MakeQa("Question " + std::to_string(i) + "a",
                                           json::array({ "D1:1" }), 1, json("answer_a")),
                                    MakeQa("Question " + std::to_string(i) + "b",
                                           json::array({ "D2:1" }), 2, json("answer_b")) });
        dataset.push_back(std::move(entry));
    }
    ASSERT_TRUE(WriteDatasetFile(dataset_path, dataset).ok());

    const LoCoMoBenchmarkLoadConfig load_config{
        .dataset_path = dataset_path,
        .sample_rate = 0.5,
        .random_seed = 999,
    };
    const absl::StatusOr<MemoryBenchmarkSuite> first = LoadLoCoMoBenchmarkSuite(load_config);
    const absl::StatusOr<MemoryBenchmarkSuite> second = LoadLoCoMoBenchmarkSuite(load_config);

    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(second.ok()) << second.status();
    ASSERT_EQ(first->cases.size(), second->cases.size());
    ASSERT_FALSE(first->cases.empty());
    for (std::size_t index = 0; index < first->cases.size(); ++index) {
        EXPECT_EQ(first->cases[index].eval_case.case_id, second->cases[index].eval_case.case_id);
    }
}

TEST(LoCoMoBenchmarkTest, RunsThroughGenericMemoryBenchmarkRunner) {
    ScopedTempPath temp_dir("locomo_run_test_");
    const std::filesystem::path dataset_path = temp_dir.path() / "locomo10.json";
    const std::filesystem::path output_dir = temp_dir.path() / "out";
    ASSERT_TRUE(WriteDatasetFile(dataset_path, json::array({ MakeConversationEntry() })).ok());

    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeMidTermAwareFakeClient("Caroline joined on 7 May 2023."),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    LoCoMoBenchmarkRunConfig run_config;
    run_config.dataset_path = dataset_path;
    run_config.output_directory = output_dir;
    run_config.case_id_filter = std::string("conv-26_q1");
    run_config.sample_rate = 1.0;
    run_config.live_gateway_port = live_gateway.port();

    const absl::StatusOr<MemoryBenchmarkReport> report = RunLoCoMoBenchmark(run_config);
    ASSERT_TRUE(report.ok()) << report.status();

    EXPECT_EQ(report->benchmark_name, "locomo");
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_EQ(report->cases.front().case_id, "conv-26_q1");
    EXPECT_TRUE(report->cases.front().passed);
    EXPECT_TRUE(std::filesystem::exists(output_dir / "report.json"));
    EXPECT_TRUE(std::filesystem::exists(output_dir / "artifacts" / "conv-26_q1.json"));
}

} // namespace
} // namespace isla::server::evals
