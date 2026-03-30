#include "isla/server/memory/sleep_cycle_semantic_extractor.hpp"

#include <memory>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "isla/server/llm_client.hpp"
#include "server/src/llm_client_mock.hpp"

namespace isla::server::memory {
namespace {

using ::isla::server::LlmEventCallback;
using ::isla::server::LlmRequest;
using ::isla::server::test::EmitLlmResponse;
using ::isla::server::test::MockLlmClient;
using ::testing::_;
using ::testing::Invoke;

Timestamp Ts(std::string_view text) {
    return nlohmann::json(text).get<Timestamp>();
}

Episode MakeEpisode() {
    return Episode{
        .episode_id = "ep_001",
        .tier1_detail = std::string("full detail"),
        .tier2_summary = "summary",
        .tier3_ref = "stub ref",
        .tier3_keywords = { "memory" },
        .salience = 6,
        .embedding = {},
        .created_at = Ts("2026-03-08T14:00:01Z"),
    };
}

TEST(SleepCycleSemanticExtractorTest, ExtractParsesStrictJsonResponse) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            EXPECT_EQ(request.model, "gpt-test");
            EXPECT_EQ(request.reasoning_effort, isla::server::LlmReasoningEffort::kMedium);
            EXPECT_THAT(request.system_prompt,
                        ::testing::HasSubstr("extract durable semantic memory candidates"));
            EXPECT_THAT(request.user_text, ::testing::HasSubstr("\"episode_id\":\"ep_001\""));
            return EmitLlmResponse(
                R"({"entities":[{"label":"Mochi","category":"pet","source_episode_ids":["ep_001"]}],"relationships":[{"from_label":"user","from_category":"person","predicate":"owns","to_label":"Mochi","to_category":"pet","evidence":"EXPLICIT_STATEMENT","source_episode_ids":["ep_001"]}]})",
                on_event);
        }));

    absl::StatusOr<SleepCycleSemanticExtractorPtr> extractor = CreateLlmSleepCycleSemanticExtractor(
        llm_client, "gpt-test", isla::server::LlmReasoningEffort::kMedium);
    ASSERT_TRUE(extractor.ok()) << extractor.status();

    absl::StatusOr<SleepCycleSemanticExtractionResult> result =
        (*extractor)
            ->Extract(SleepCycleSemanticExtractionRequest{
                .user_id = "user_001",
                .mid_term_episodes = { MakeEpisode() },
            });

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->entities.size(), 1U);
    EXPECT_EQ(result->entities.front().label, "Mochi");
    EXPECT_EQ(result->entities.front().category, "pet");
    ASSERT_EQ(result->relationships.size(), 1U);
    EXPECT_EQ(result->relationships.front().predicate, "owns");
    EXPECT_EQ(result->relationships.front().evidence,
              SemanticRelationshipEvidence::ExplicitStatement);
}

TEST(SleepCycleSemanticExtractorTest, ExtractAcceptsCodeFencedJson) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse("```json\n{\"entities\":[],\"relationships\":[]}\n```",
                                   on_event);
        }));

    absl::StatusOr<SleepCycleSemanticExtractorPtr> extractor =
        CreateLlmSleepCycleSemanticExtractor(llm_client, "gpt-test");
    ASSERT_TRUE(extractor.ok()) << extractor.status();

    absl::StatusOr<SleepCycleSemanticExtractionResult> result =
        (*extractor)
            ->Extract(SleepCycleSemanticExtractionRequest{
                .user_id = "user_001",
                .mid_term_episodes = { MakeEpisode() },
            });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->entities.empty());
    EXPECT_TRUE(result->relationships.empty());
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsUnknownSourceEpisodeIds) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse(
                R"({"entities":[{"label":"Mochi","category":"pet","source_episode_ids":["ep_missing"]}],"relationships":[]})",
                on_event);
        }));

    absl::StatusOr<SleepCycleSemanticExtractorPtr> extractor =
        CreateLlmSleepCycleSemanticExtractor(llm_client, "gpt-test");
    ASSERT_TRUE(extractor.ok()) << extractor.status();

    absl::StatusOr<SleepCycleSemanticExtractionResult> result =
        (*extractor)
            ->Extract(SleepCycleSemanticExtractionRequest{
                .user_id = "user_001",
                .mid_term_episodes = { MakeEpisode() },
            });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(), ::testing::HasSubstr("unknown source_episode_id"));
}

} // namespace
} // namespace isla::server::memory
