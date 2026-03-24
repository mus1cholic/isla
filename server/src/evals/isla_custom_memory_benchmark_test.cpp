#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::memory::ConversationMessageWrite;
using isla::server::memory::Episode;
using isla::server::memory::LoadPrompt;
using isla::server::memory::MemorySessionRecord;
using isla::server::memory::MemoryStore;
using isla::server::memory::MemoryStoreSnapshot;
using isla::server::memory::PromptAsset;

std::string BuildBenchmarkReply(std::string_view user_text) {
    if (user_text.find("tea") != std::string_view::npos) {
        return "genmaicha";
    }
    if (user_text.find("language") != std::string_view::npos) {
        return "Rust";
    }
    if (user_text.find("editor") != std::string_view::npos) {
        return "Neovim";
    }
    if (user_text.find("project") != std::string_view::npos) {
        return "Atlas renderer";
    }
    if (user_text.find("exact command") != std::string_view::npos) {
        return "bazel test //server/src:eval_runner_test";
    }
    if (user_text.find("Osaka trip") != std::string_view::npos) {
        return "2026-03-14T10:00:00Z";
    }
    return "placeholder benchmark reply";
}

std::string BuildMidTermAwareResponse(std::string_view system_prompt, std::string_view user_text) {
    if (system_prompt.find("should_flush") != std::string_view::npos) {
        return R"({"should_flush":false,"item_id":null,"split_at":null,"reasoning":"test"})";
    }
    if (system_prompt.find("tier2_summary") != std::string_view::npos) {
        return R"({"tier1_detail":"d","tier2_summary":"s","tier3_ref":"r","tier3_keywords":["k"],"salience":5})";
    }
    return BuildBenchmarkReply(user_text);
}

class FakeLiveOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        const absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = BuildMidTermAwareResponse(request.system_prompt, request.user_text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{ .response_id = "fake_response_id" });
    }
};

struct RecordedOpenAiRequest {
    std::string system_prompt;
    std::string user_text;
};

class RecordingFakeLiveOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(RecordedOpenAiRequest{
                .system_prompt = request.system_prompt,
                .user_text = request.user_text,
            });
        }

        const absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = BuildMidTermAwareResponse(request.system_prompt, request.user_text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(
            OpenAiResponsesCompletedEvent{ .response_id = "recording_fake_response_id" });
    }

    [[nodiscard]] std::vector<RecordedOpenAiRequest> RecordedRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::vector<RecordedOpenAiRequest> requests_;
};

class FakeLiveLlmClient final : public isla::server::LlmClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const isla::server::LlmRequest& request,
                   const isla::server::LlmEventCallback& on_event) const override {
        const absl::Status delta_status = on_event(isla::server::LlmTextDeltaEvent{
            .text_delta = BuildMidTermAwareResponse(request.system_prompt, request.user_text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(isla::server::LlmCompletedEvent{
            .response_id = "fake_llm_response_id",
        });
    }
};

struct RecordedLlmRequest {
    std::string system_prompt;
    std::string user_text;
};

class RecordingFakeLiveLlmClient final : public isla::server::LlmClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const isla::server::LlmRequest& request,
                   const isla::server::LlmEventCallback& on_event) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(RecordedLlmRequest{
                .system_prompt = request.system_prompt,
                .user_text = request.user_text,
            });
        }

        const absl::Status delta_status = on_event(isla::server::LlmTextDeltaEvent{
            .text_delta = BuildMidTermAwareResponse(request.system_prompt, request.user_text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
        return on_event(isla::server::LlmCompletedEvent{
            .response_id = "recording_fake_llm_response_id",
        });
    }

    [[nodiscard]] std::vector<RecordedLlmRequest> RecordedRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::vector<RecordedLlmRequest> requests_;
};

class ScopedOutputDirectory {
  public:
    ScopedOutputDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("isla_custom_memory_benchmark_test_" +
                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))) {}

    ~ScopedOutputDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

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

std::shared_ptr<const OpenAiResponsesClient> CreateFakeLiveClient() {
    return std::make_shared<const FakeLiveOpenAiResponsesClient>();
}

std::shared_ptr<const isla::server::LlmClient> CreateFakeLiveLlmClient() {
    return std::make_shared<const FakeLiveLlmClient>();
}

TEST(IslaCustomMemoryBenchmarkTest, MissingLiveGatewayPortMarksAllCasesFailed) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{});

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 6U);
    EXPECT_EQ(report->passed_cases, 0U);
    EXPECT_EQ(report->failed_cases, 6U);
    ASSERT_EQ(report->cases.size(), 6U);
    for (const IslaCustomMemoryCaseReport& case_report : report->cases) {
        EXPECT_FALSE(case_report.passed);
        EXPECT_FALSE(case_report.final_reply.has_value());
        EXPECT_FALSE(case_report.artifact_path.has_value());
        ASSERT_TRUE(case_report.failure.has_value());
        EXPECT_EQ(case_report.failure->code, "invalid_argument");
        EXPECT_FALSE(case_report.failure->message.empty());
    }
}

TEST(IslaCustomMemoryBenchmarkTest, RejectsUnknownCaseIdBeforeGatewayValidation) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .case_id_filter = std::string("missing_case"),
        });

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(report.status().message()).find("case_id_filter"), std::string::npos);
}

TEST(IslaCustomMemoryBenchmarkTest, UnreachableLiveGatewayMarksAllCasesFailed) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .live_gateway_host = "127.0.0.1",
            .live_gateway_port = 1,
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 6U);
    EXPECT_EQ(report->passed_cases, 0U);
    EXPECT_EQ(report->failed_cases, 6U);
    ASSERT_EQ(report->cases.size(), 6U);
    for (const IslaCustomMemoryCaseReport& case_report : report->cases) {
        EXPECT_FALSE(case_report.passed);
        EXPECT_FALSE(case_report.final_reply.has_value());
        EXPECT_FALSE(case_report.artifact_path.has_value());
        ASSERT_TRUE(case_report.failure.has_value());
        EXPECT_TRUE(case_report.failure->code == "unavailable" ||
                    case_report.failure->code == "deadline_exceeded");
        EXPECT_FALSE(case_report.failure->message.empty());
    }
}

TEST(IslaCustomMemoryBenchmarkTest, RunsAllCasesAndPersistsArtifactsWithLiveGateway) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = std::chrono::milliseconds(0),
        .openai_client = CreateFakeLiveClient(),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .live_gateway_port = live_gateway.port(),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 6U);
    EXPECT_EQ(report->passed_cases, 6U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 6U);

    for (const IslaCustomMemoryCaseReport& case_report : report->cases) {
        EXPECT_TRUE(case_report.passed) << case_report.case_id;
        EXPECT_EQ(case_report.final_answer_evaluation, "unimplemented");
        EXPECT_TRUE(case_report.final_reply.has_value()) << case_report.case_id;
        EXPECT_TRUE(case_report.artifact_path.has_value()) << case_report.case_id;
        EXPECT_FALSE(case_report.failure.has_value()) << case_report.case_id;
    }

    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "report.json"));
    EXPECT_TRUE(
        std::filesystem::exists(output_directory.path() / "artifacts" / "direct_fact_recall.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" /
                                        "recency_contradiction.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" /
                                        "mid_term_visibility.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" /
                                        "expandable_exact_detail.json"));
    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "artifacts" /
                                        "relative_timestamp_recall.json"));

    std::ifstream report_file(output_directory.path() / "report.json");
    ASSERT_TRUE(report_file.is_open());
    const nlohmann::json report_json = nlohmann::json::parse(report_file);
    ASSERT_TRUE(report_json.contains("cases"));
    ASSERT_EQ(report_json["cases"].size(), 6U);
    EXPECT_TRUE(report_json["cases"][0]["failure"].is_null());
}

TEST(IslaCustomMemoryBenchmarkTest, SupportsFilteringToOneCaseWithLiveGatewayOpenAi) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = std::chrono::milliseconds(0),
        .openai_client = CreateFakeLiveClient(),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("direct_fact_recall_tea"),
            .live_gateway_port = live_gateway.port(),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_EQ(report->cases.front().suite_id, "direct_fact_recall");
    EXPECT_EQ(report->cases.front().case_id, "direct_fact_recall_tea");
    EXPECT_TRUE(report->cases.front().passed);
    EXPECT_TRUE(report->cases.front().final_reply.has_value());
    EXPECT_TRUE(report->cases.front().artifact_path.has_value());
    EXPECT_FALSE(report->cases.front().failure.has_value());

    EXPECT_TRUE(std::filesystem::exists(output_directory.path() / "report.json"));
    EXPECT_TRUE(
        std::filesystem::exists(output_directory.path() / "artifacts" / "direct_fact_recall.json"));

    std::size_t artifact_count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(output_directory.path() / "artifacts")) {
        if (entry.is_regular_file()) {
            artifact_count += 1U;
        }
    }
    EXPECT_EQ(artifact_count, 1U);
}

TEST(IslaCustomMemoryBenchmarkTest, SupportsFilteringToOneCaseWithLiveGatewayGenericLlm) {
    ScopedOutputDirectory output_directory;
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = std::chrono::milliseconds(0),
        .llm_client = CreateFakeLiveLlmClient(),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("direct_fact_recall_tea"),
            .live_gateway_port = live_gateway.port(),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);
    ASSERT_TRUE(report->cases.front().final_reply.has_value());
    EXPECT_EQ(*report->cases.front().final_reply, "genmaicha");
    EXPECT_FALSE(report->cases.front().failure.has_value());
}

TEST(IslaCustomMemoryBenchmarkTest, PersistsConversationThroughGatewayMemoryStore) {
    ScopedOutputDirectory output_directory;
    auto store = std::make_shared<RecordingMemoryStore>();
    ScopedLiveGatewayServer live_gateway(GatewayStubResponderConfig{
        .response_delay = std::chrono::milliseconds(0),
        .memory_store = store,
        .openai_client = CreateFakeLiveClient(),
    });
    ASSERT_TRUE(live_gateway.Start().ok());

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("mid_term_visibility"),
            .live_gateway_port = live_gateway.port(),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);
    EXPECT_FALSE(report->cases.front().failure.has_value());
    EXPECT_FALSE(store->session_records.empty());
    EXPECT_GE(store->message_writes.size(), 4U);
}

} // namespace
} // namespace isla::server::evals
