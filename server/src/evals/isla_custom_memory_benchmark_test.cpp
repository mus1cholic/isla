#include "isla/server/evals/isla_custom_memory_benchmark.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include <gtest/gtest.h>

#include "isla/server/llm_client.hpp"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::memory::LoadPrompt;
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
        const absl::Status delta_status = on_event(
            OpenAiResponsesTextDeltaEvent{ .text_delta = BuildBenchmarkReply(request.user_text) });
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

        const absl::Status delta_status = on_event(
            OpenAiResponsesTextDeltaEvent{ .text_delta = BuildBenchmarkReply(request.user_text) });
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
            .text_delta = BuildBenchmarkReply(request.user_text),
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
            .text_delta = BuildBenchmarkReply(request.user_text),
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

std::shared_ptr<const OpenAiResponsesClient> CreateFakeLiveClient() {
    return std::make_shared<const FakeLiveOpenAiResponsesClient>();
}

std::shared_ptr<const isla::server::LlmClient> CreateFakeLiveLlmClient() {
    return std::make_shared<const FakeLiveLlmClient>();
}

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

TEST(IslaCustomMemoryBenchmarkTest, RejectsAmbiguousInjectedProviderClients) {
    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .case_id_filter = std::string("direct_fact_recall_tea"),
            .live_llm_client = CreateFakeLiveLlmClient(),
            .live_openai_client = CreateFakeLiveClient(),
        });

    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(report.status().message()).find("mutually exclusive"), std::string::npos);
}

TEST(IslaCustomMemoryBenchmarkTest, RunsAllCasesAndPersistsArtifactsWithInjectedLiveClient) {
    ScopedOutputDirectory output_directory;

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .live_openai_client = CreateFakeLiveClient(),
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
}

TEST(IslaCustomMemoryBenchmarkTest, SupportsFilteringToOneCaseWithInjectedLiveClient) {
    ScopedOutputDirectory output_directory;

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("direct_fact_recall_tea"),
            .live_openai_client = CreateFakeLiveClient(),
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

TEST(IslaCustomMemoryBenchmarkTest, SupportsFilteringToOneCaseWithInjectedGenericLlmClient) {
    ScopedOutputDirectory output_directory;

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("direct_fact_recall_tea"),
            .live_llm_client = CreateFakeLiveLlmClient(),
        });

    ASSERT_TRUE(report.ok()) << report.status();
    EXPECT_EQ(report->total_cases, 1U);
    EXPECT_EQ(report->passed_cases, 1U);
    EXPECT_EQ(report->failed_cases, 0U);
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);
    ASSERT_TRUE(report->cases.front().final_reply.has_value());
    EXPECT_EQ(*report->cases.front().final_reply, "genmaicha");
}

TEST(IslaCustomMemoryBenchmarkTest, WrapperKeepsMidTermPromptsOffInjectedLiveClient) {
    ScopedOutputDirectory output_directory;
    auto recording_client = std::make_shared<RecordingFakeLiveOpenAiResponsesClient>();

    const absl::StatusOr<std::string> decider_prompt =
        LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    const absl::StatusOr<std::string> compactor_prompt =
        LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("mid_term_visibility"),
            .live_openai_client = recording_client,
        });

    ASSERT_TRUE(report.ok()) << report.status();
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);

    const std::vector<RecordedOpenAiRequest> requests = recording_client->RecordedRequests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(requests.front().user_text.find("What project were we debugging earlier?"),
              std::string::npos);
    EXPECT_FALSE(requests.front().system_prompt.empty());
    EXPECT_NE(requests.front().system_prompt, *decider_prompt);
    EXPECT_NE(requests.front().system_prompt, *compactor_prompt);
}

TEST(IslaCustomMemoryBenchmarkTest, WrapperKeepsMidTermPromptsOffInjectedGenericLlmClient) {
    ScopedOutputDirectory output_directory;
    auto recording_client = std::make_shared<RecordingFakeLiveLlmClient>();

    const absl::StatusOr<std::string> decider_prompt =
        LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    const absl::StatusOr<std::string> compactor_prompt =
        LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    const absl::StatusOr<IslaCustomMemoryBenchmarkReport> report =
        RunIslaCustomMemoryBenchmark(IslaCustomMemoryBenchmarkRunConfig{
            .output_directory = output_directory.path(),
            .case_id_filter = std::string("mid_term_visibility"),
            .live_llm_client = recording_client,
        });

    ASSERT_TRUE(report.ok()) << report.status();
    ASSERT_EQ(report->cases.size(), 1U);
    EXPECT_TRUE(report->cases.front().passed);

    const std::vector<RecordedLlmRequest> requests = recording_client->RecordedRequests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(requests.front().user_text.find("What project were we debugging earlier?"),
              std::string::npos);
    EXPECT_FALSE(requests.front().system_prompt.empty());
    EXPECT_NE(requests.front().system_prompt, *decider_prompt);
    EXPECT_NE(requests.front().system_prompt, *compactor_prompt);
}

} // namespace
} // namespace isla::server::evals
