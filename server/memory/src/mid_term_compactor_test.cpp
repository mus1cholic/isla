#include "isla/server/memory/mid_term_compactor.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/prompt_loader.hpp"

namespace isla::server::memory {
namespace {

using isla::server::LlmClient;
using isla::server::LlmCompletedEvent;
using isla::server::LlmEventCallback;
using isla::server::LlmRequest;
using isla::server::LlmTextDeltaEvent;
using nlohmann::json;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

class FakeLlmClient final : public LlmClient {
  public:
    explicit FakeLlmClient(std::string canned_response)
        : canned_response_(std::move(canned_response)) {}

    FakeLlmClient(std::string canned_response, absl::Status stream_status)
        : canned_response_(std::move(canned_response)), stream_status_(std::move(stream_status)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status StreamResponse(const LlmRequest& request,
                                              const LlmEventCallback& on_event) const override {
        last_request_model_ = request.model;
        last_request_system_prompt_ = request.system_prompt;
        last_request_user_text_ = request.user_text;

        if (!stream_status_.ok()) {
            return stream_status_;
        }

        if (!canned_response_.empty()) {
            const absl::Status first_status = on_event(LlmTextDeltaEvent{
                .text_delta = canned_response_.substr(0, canned_response_.size() / 2U) });
            if (!first_status.ok()) {
                return first_status;
            }
            const absl::Status second_status = on_event(LlmTextDeltaEvent{
                .text_delta = canned_response_.substr(canned_response_.size() / 2U) });
            if (!second_status.ok()) {
                return second_status;
            }
        }
        return on_event(LlmCompletedEvent{ .response_id = "resp_test" });
    }

    [[nodiscard]] const std::string& last_request_model() const {
        return last_request_model_;
    }

    [[nodiscard]] const std::string& last_request_system_prompt() const {
        return last_request_system_prompt_;
    }

    [[nodiscard]] const std::string& last_request_user_text() const {
        return last_request_user_text_;
    }

  private:
    std::string canned_response_;
    absl::Status stream_status_ = absl::OkStatus();
    mutable std::string last_request_model_;
    mutable std::string last_request_system_prompt_;
    mutable std::string last_request_user_text_;
};

struct CompactorWithFake {
    std::shared_ptr<FakeLlmClient> fake_client;
    MidTermCompactorPtr compactor;
};

MidTermCompactionRequest MakeCompactionRequest() {
    return MidTermCompactionRequest{
        .session_id = "session_test",
        .flush_candidate =
            OngoingEpisodeFlushCandidate{
                .conversation_item_index = 2U,
                .ongoing_episode =
                    OngoingEpisode{
                        .messages = {
                            Message{ .role = MessageRole::User,
                                     .content = "Please help me debug the export crash.",
                                     .create_time = Ts("2026-03-16T10:00:00Z") },
                            Message{ .role = MessageRole::Assistant,
                                     .content = "Share the stack trace and file if you have it.",
                                     .create_time = Ts("2026-03-16T10:00:10Z") },
                            Message{ .role = MessageRole::User,
                                     .content = "It points to export_report.cpp line 214.",
                                     .create_time = Ts("2026-03-16T10:00:20Z") },
                        },
                    },
            },
    };
}

CompactorWithFake MakeCompactor(std::string canned_response) {
    auto fake = std::make_shared<FakeLlmClient>(std::move(canned_response));
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "test-model");
    if (!compactor.ok()) {
        return { .fake_client = fake, .compactor = nullptr };
    }
    return { .fake_client = fake, .compactor = std::move(*compactor) };
}

CompactorWithFake MakeFailingCompactor(absl::Status failure_status) {
    auto fake = std::make_shared<FakeLlmClient>("", std::move(failure_status));
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "test-model");
    if (!compactor.ok()) {
        return { .fake_client = fake, .compactor = nullptr };
    }
    return { .fake_client = fake, .compactor = std::move(*compactor) };
}

TEST(LlmMidTermCompactorTest, CompactReturnsValidTieredOutput) {
    const std::string response = R"({
        "tier1_detail": "The crash was in export_report.cpp line 214 after a missing optional.",
        "tier2_summary": "The discussion focused on debugging an export crash. The root cause was narrowed to a missing optional in export_report.cpp. The next step was to guard the missing value and add a regression test.",
        "tier3_ref": "Debugged an export crash in export_report.cpp caused by a missing optional.",
        "tier3_keywords": ["export crash", "export_report.cpp", "optional", "debugging", "regression test"],
        "salience": 8
    })";
    auto [fake, compactor] = MakeCompactor(response);
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    ASSERT_TRUE(compacted->tier1_detail.has_value());
    EXPECT_EQ(*compacted->tier1_detail,
              "The crash was in export_report.cpp line 214 after a missing optional.");
    EXPECT_EQ(compacted->tier2_summary,
              "The discussion focused on debugging an export crash. The root cause was narrowed "
              "to a missing optional in export_report.cpp. The next step was to guard the "
              "missing value and add a regression test.");
    EXPECT_EQ(compacted->tier3_ref,
              "Debugged an export crash in export_report.cpp caused by a missing optional.");
    EXPECT_EQ(compacted->tier3_keywords,
              (std::vector<std::string>{ "export crash", "export_report.cpp", "optional",
                                         "debugging", "regression test" }));
    EXPECT_EQ(compacted->salience, 8);
    EXPECT_TRUE(compacted->embedding.empty());
}

TEST(LlmMidTermCompactorTest, CompactAcceptsNullTier1Detail) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "The exchange was a short coordination step about the next task.",
        "tier3_ref": "Coordinated the next debugging step.",
        "tier3_keywords": ["coordination", "debugging", "next step", "task", "planning"],
        "salience": 4
    })";
    auto [fake, compactor] = MakeCompactor(response);
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    EXPECT_FALSE(compacted->tier1_detail.has_value());
}

TEST(LlmMidTermCompactorTest, CompactPassesCorrectJsonToLlm) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })";
    auto [fake, compactor] = MakeCompactor(response);
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(compacted.ok()) << compacted.status();

    const json sent = json::parse(fake->last_request_user_text());
    ASSERT_TRUE(sent.contains("messages"));
    ASSERT_EQ(sent["messages"].size(), 3U);
    EXPECT_EQ(sent["messages"][0]["role"], "user");
    EXPECT_EQ(sent["messages"][0]["content"], "Please help me debug the export crash.");
    EXPECT_EQ(sent["messages"][1]["role"], "assistant");
    EXPECT_EQ(sent["messages"][1]["content"], "Share the stack trace and file if you have it.");
    EXPECT_FALSE(sent["messages"][0].contains("create_time"));
    EXPECT_EQ(sent["messages"][0].size(), 2U);
}

TEST(LlmMidTermCompactorTest, CompactHandlesEmptyEpisodeMessages) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "No substantive content was available to summarize.",
        "tier3_ref": "Compacted an empty episode.",
        "tier3_keywords": ["empty episode", "compaction", "memory", "llm", "summary"],
        "salience": 1
    })";
    auto [fake, compactor] = MakeCompactor(response);
    ASSERT_NE(compactor, nullptr);

    MidTermCompactionRequest request = MakeCompactionRequest();
    request.flush_candidate.ongoing_episode.messages.clear();

    const absl::StatusOr<CompactedMidTermEpisode> compacted = compactor->Compact(request);

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    const json sent = json::parse(fake->last_request_user_text());
    ASSERT_TRUE(sent.contains("messages"));
    EXPECT_TRUE(sent["messages"].empty());
}

TEST(LlmMidTermCompactorTest, CompactUsesCorrectModelAndSystemPrompt) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })";
    auto [fake, compactor] = MakeCompactor(response);
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(compacted.ok()) << compacted.status();

    EXPECT_EQ(fake->last_request_model(), "test-model");
    const absl::StatusOr<std::string> expected_prompt =
        LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(expected_prompt.ok()) << expected_prompt.status();
    EXPECT_EQ(fake->last_request_system_prompt(), *expected_prompt);
}

TEST(LlmMidTermCompactorTest, CompactRejectsInvalidJson) {
    auto [fake, compactor] = MakeCompactor("not json");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsMissingFields) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsWrongFieldTypes) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": [],
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsExtraFields) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5,
        "extra": "nope"
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyRequiredStrings) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyTier3Ref) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyTier1DetailWhenPresent) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": "",
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsSalienceBelowRange) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 0
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsSalienceAboveRange) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 11
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsNonIntegerSalience) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5.5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsWrongKeywordCount) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyKeywordEntry) {
    auto [fake, compactor] = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactPropagatesLlmFailure) {
    auto [fake, compactor] = MakeFailingCompactor(absl::InternalError("LLM service unavailable"));
    ASSERT_NE(compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInternal);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyLlmResponse) {
    auto fake = std::make_shared<FakeLlmClient>("");
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "test-model");
    ASSERT_TRUE(compactor.ok()) << compactor.status();
    ASSERT_NE(*compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        (*compactor)->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, FactorySucceedsWithValidInputs) {
    auto fake = std::make_shared<FakeLlmClient>(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "gpt-4o-mini");

    ASSERT_TRUE(compactor.ok()) << compactor.status();
    EXPECT_NE(*compactor, nullptr);
}

TEST(LlmMidTermCompactorTest, FactoryFailsForNullClient) {
    absl::StatusOr<MidTermCompactorPtr> compactor =
        CreateLlmMidTermCompactor(nullptr, "test-model");

    ASSERT_FALSE(compactor.ok());
    EXPECT_EQ(compactor.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, FactoryFailsForEmptyModel) {
    auto fake = std::make_shared<FakeLlmClient>("");
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "");

    ASSERT_FALSE(compactor.ok());
    EXPECT_EQ(compactor.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server::memory
