#include "isla/server/memory/mid_term_compactor.hpp"

#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "isla/server/embedding_client.hpp"
#include "isla/server/memory/memory_types.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "server/src/embedding_client_mock.hpp"
#include "server/src/llm_client_mock.hpp"

namespace isla::server::memory {
namespace {

using isla::server::EmbeddingRequest;
using isla::server::LlmRequest;
using nlohmann::json;
using ::testing::_;
using ::testing::Return;

Timestamp Ts(std::string_view text) {
    return json(text).get<Timestamp>();
}

struct CompactorWithFake {
    std::shared_ptr<isla::server::test::MockLlmClient> fake_client;
    std::shared_ptr<isla::server::test::MockEmbeddingClient> fake_embedding_client;
    std::shared_ptr<LlmRequest> last_request;
    std::shared_ptr<EmbeddingRequest> last_embedding_request;
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

CompactorWithFake MakeCompactor(
    std::string canned_response,
    const std::shared_ptr<isla::server::test::MockEmbeddingClient>& embedding_client = nullptr,
    std::string embedding_model = "",
    std::optional<isla::server::LlmReasoningEffort> reasoning_effort = std::nullopt) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto last_request = std::make_shared<LlmRequest>();
    auto last_embedding_request = std::make_shared<EmbeddingRequest>();
    EXPECT_CALL(*fake, Validate()).Times(0);
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([last_request, canned_response = std::move(canned_response)](
                      const LlmRequest& request, const isla::server::LlmEventCallback& on_event) {
            *last_request = request;
            const std::size_t midpoint = canned_response.size() / 2U;
            absl::Status first_status = on_event(isla::server::LlmTextDeltaEvent{
                .text_delta = canned_response.substr(0, midpoint),
            });
            if (!first_status.ok()) {
                return first_status;
            }
            return isla::server::test::EmitLlmResponse(canned_response.substr(midpoint), on_event);
        });
    if (embedding_client != nullptr) {
        EXPECT_CALL(*embedding_client, Validate()).Times(0);
        EXPECT_CALL(*embedding_client, Embed(_))
            .WillRepeatedly([last_embedding_request](
                                const EmbeddingRequest& request) -> absl::StatusOr<Embedding> {
                *last_embedding_request = request;
                return Embedding{ 0.1, 0.2, 0.3 };
            });
    }
    absl::StatusOr<MidTermCompactorPtr> compactor =
        reasoning_effort.has_value()
            ? CreateLlmMidTermCompactor(fake, "test-model", embedding_client,
                                        std::move(embedding_model), *reasoning_effort)
            : CreateLlmMidTermCompactor(fake, "test-model", embedding_client,
                                        std::move(embedding_model));
    if (!compactor.ok()) {
        return { .fake_client = fake,
                 .fake_embedding_client = embedding_client,
                 .last_request = last_request,
                 .last_embedding_request = last_embedding_request,
                 .compactor = nullptr };
    }
    return { .fake_client = fake,
             .fake_embedding_client = embedding_client,
             .last_request = last_request,
             .last_embedding_request = last_embedding_request,
             .compactor = std::move(*compactor) };
}

CompactorWithFake MakeFailingCompactor(absl::Status failure_status) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto last_request = std::make_shared<LlmRequest>();
    auto last_embedding_request = std::make_shared<EmbeddingRequest>();
    EXPECT_CALL(*fake, Validate()).Times(0);
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([last_request, failure_status = std::move(failure_status)](
                      const LlmRequest& request,
                      const isla::server::LlmEventCallback& on_event) -> absl::Status {
            static_cast<void>(on_event);
            *last_request = request;
            return failure_status;
        });
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "test-model");
    if (!compactor.ok()) {
        return { .fake_client = fake,
                 .fake_embedding_client = nullptr,
                 .last_request = last_request,
                 .last_embedding_request = last_embedding_request,
                 .compactor = nullptr };
    }
    return { .fake_client = fake,
             .fake_embedding_client = nullptr,
             .last_request = last_request,
             .last_embedding_request = last_embedding_request,
             .compactor = std::move(*compactor) };
}

TEST(LlmMidTermCompactorTest, CompactReturnsValidTieredOutput) {
    const std::string response = R"({
        "tier1_detail": "The crash was in export_report.cpp line 214 after a missing optional.",
        "tier2_summary": "The discussion focused on debugging an export crash. The root cause was narrowed to a missing optional in export_report.cpp. The next step was to guard the missing value and add a regression test.",
        "tier3_ref": "Debugged an export crash in export_report.cpp caused by a missing optional.",
        "tier3_keywords": ["export crash", "export_report.cpp", "optional", "debugging", "regression test"],
        "salience": 8
    })";
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

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
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

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
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(compacted.ok()) << compacted.status();

    const json sent = json::parse(built.last_request->user_text);
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
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    MidTermCompactionRequest request = MakeCompactionRequest();
    request.flush_candidate.ongoing_episode.messages.clear();

    const absl::StatusOr<CompactedMidTermEpisode> compacted = built.compactor->Compact(request);

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    const json sent = json::parse(built.last_request->user_text);
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
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(compacted.ok()) << compacted.status();

    EXPECT_EQ(built.last_request->model, "test-model");
    const absl::StatusOr<std::string> expected_prompt =
        LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(expected_prompt.ok()) << expected_prompt.status();
    EXPECT_EQ(built.last_request->system_prompt, *expected_prompt);
}

TEST(LlmMidTermCompactorTest, CompactRejectsInvalidJson) {
    const CompactorWithFake built = MakeCompactor("not json");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsMissingFields) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsWrongFieldTypes) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": [],
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsExtraFields) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5,
        "extra": "nope"
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyRequiredStrings) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyTier3Ref) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyTier1DetailWhenPresent) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": "",
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsSalienceBelowRange) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 0
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsSalienceAboveRange) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 11
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsNonIntegerSalience) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four", "five"],
        "salience": 5.5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsWrongKeywordCount) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "two", "three", "four"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyKeywordEntry) {
    const CompactorWithFake built = MakeCompactor(R"({
        "tier1_detail": null,
        "tier2_summary": "Summary text.",
        "tier3_ref": "Reference sentence.",
        "tier3_keywords": ["one", "", "three", "four", "five"],
        "salience": 5
    })");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactPropagatesLlmFailure) {
    const CompactorWithFake built =
        MakeFailingCompactor(absl::InternalError("LLM service unavailable"));
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInternal);
}

TEST(LlmMidTermCompactorTest, CompactGeneratesEmbeddingWhenEmbeddingClientConfigured) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "The discussion focused on debugging an export crash.",
        "tier3_ref": "Debugged an export crash.",
        "tier3_keywords": ["export", "crash", "debug", "summary", "memory"],
        "salience": 6
    })";
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();
    const CompactorWithFake built =
        MakeCompactor(response, embedding_client, "gemini-embedding-2-preview");
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    EXPECT_EQ(compacted->embedding, (Embedding{ 0.1, 0.2, 0.3 }));
    EXPECT_EQ(built.last_embedding_request->model, "gemini-embedding-2-preview");
    EXPECT_EQ(built.last_embedding_request->text,
              "The discussion focused on debugging an export crash.");
}

TEST(LlmMidTermCompactorTest, CompactPropagatesEmbeddingFailure) {
    const std::string response = R"({
        "tier1_detail": null,
        "tier2_summary": "The discussion focused on debugging an export crash.",
        "tier3_ref": "Debugged an export crash.",
        "tier3_keywords": ["export", "crash", "debug", "summary", "memory"],
        "salience": 6
    })";
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();
    EXPECT_CALL(*fake, Validate()).Times(0);
    EXPECT_CALL(*embedding_client, Validate()).Times(0);
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce(
            [response](const LlmRequest& request, const isla::server::LlmEventCallback& on_event) {
                static_cast<void>(request);
                return isla::server::test::EmitLlmResponse(response, on_event);
            });
    EXPECT_CALL(*embedding_client, Embed(_))
        .WillOnce(Return(absl::UnavailableError("embedding service unavailable")));
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(
        fake, "test-model", embedding_client, "gemini-embedding-2-preview");
    ASSERT_TRUE(compactor.ok()) << compactor.status();

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        (*compactor)->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kUnavailable);
}

TEST(LlmMidTermCompactorTest, CompactRejectsEmptyLlmResponse) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    EXPECT_CALL(*fake, StreamResponse(_, _))
        .WillOnce([](const LlmRequest& request, const isla::server::LlmEventCallback& on_event) {
            static_cast<void>(request);
            return isla::server::test::EmitLlmResponse("", on_event);
        });
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "test-model");
    ASSERT_TRUE(compactor.ok()) << compactor.status();
    ASSERT_NE(*compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        (*compactor)->Compact(MakeCompactionRequest());

    ASSERT_FALSE(compacted.ok());
    EXPECT_EQ(compacted.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, CompactStripsMarkdownCodeFencesAndRetries) {
    const std::string response = "```json\n"
                                 R"({
        "tier1_detail": "Stripped from code fences.",
        "tier2_summary": "The model wrapped JSON in markdown fences.",
        "tier3_ref": "Handled a code-fenced LLM response.",
        "tier3_keywords": ["markdown", "code fence", "json", "fallback", "parse"],
        "salience": 5
    })"
                                 "\n```";
    const CompactorWithFake built = MakeCompactor(response);
    ASSERT_NE(built.compactor, nullptr);

    const absl::StatusOr<CompactedMidTermEpisode> compacted =
        built.compactor->Compact(MakeCompactionRequest());

    ASSERT_TRUE(compacted.ok()) << compacted.status();
    ASSERT_TRUE(compacted->tier1_detail.has_value());
    EXPECT_EQ(*compacted->tier1_detail, "Stripped from code fences.");
    EXPECT_EQ(compacted->salience, 5);
}

TEST(LlmMidTermCompactorTest, FactorySucceedsWithValidInputs) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
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
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    absl::StatusOr<MidTermCompactorPtr> compactor = CreateLlmMidTermCompactor(fake, "");

    ASSERT_FALSE(compactor.ok());
    EXPECT_EQ(compactor.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, FactoryFailsForEmbeddingClientWithoutEmbeddingModel) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();
    auto embedding_client = std::make_shared<isla::server::test::MockEmbeddingClient>();

    const absl::StatusOr<MidTermCompactorPtr> compactor =
        CreateLlmMidTermCompactor(fake, "test-model", embedding_client, "");

    ASSERT_FALSE(compactor.ok());
    EXPECT_EQ(compactor.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, FactoryFailsForEmbeddingModelWithoutEmbeddingClient) {
    auto fake = std::make_shared<isla::server::test::MockLlmClient>();

    const absl::StatusOr<MidTermCompactorPtr> compactor =
        CreateLlmMidTermCompactor(fake, "test-model", nullptr, "gemini-embedding-2-preview");

    ASSERT_FALSE(compactor.ok());
    EXPECT_EQ(compactor.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LlmMidTermCompactorTest, DefaultReasoningEffortIsNone) {
    auto [fake_client, fake_embedding_client, last_request, last_embedding_request, compactor] =
        MakeCompactor(R"json({
            "tier1_detail": "Detail.",
            "tier2_summary": "Summary.",
            "tier3_ref": "Ref.",
            "tier3_keywords": ["a", "b", "c", "d", "e"],
            "salience": 5
        })json");
    ASSERT_NE(compactor, nullptr);

    const auto result = compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(last_request->reasoning_effort, isla::server::LlmReasoningEffort::kNone);
}

TEST(LlmMidTermCompactorTest, ReasoningEffortIsConfigurable) {
    auto [fake_client, fake_embedding_client, last_request, last_embedding_request, compactor] =
        MakeCompactor(R"json({
            "tier1_detail": "Detail.",
            "tier2_summary": "Summary.",
            "tier3_ref": "Ref.",
            "tier3_keywords": ["a", "b", "c", "d", "e"],
            "salience": 5
        })json",
                      nullptr, "", isla::server::LlmReasoningEffort::kLow);
    ASSERT_NE(compactor, nullptr);

    const auto result = compactor->Compact(MakeCompactionRequest());
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(last_request->reasoning_effort, isla::server::LlmReasoningEffort::kLow);
}

} // namespace
} // namespace isla::server::memory
