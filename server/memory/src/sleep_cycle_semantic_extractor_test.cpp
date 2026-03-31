#include "isla/server/memory/sleep_cycle_semantic_extractor.hpp"

#include <memory>
#include <sstream>
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

std::string RepeatJsonObjectList(std::string_view object_text, std::size_t count) {
    std::ostringstream stream;
    stream << "[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0U) {
            stream << ",";
        }
        stream << object_text;
    }
    stream << "]";
    return stream.str();
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
            EXPECT_THAT(request.user_text, ::testing::HasSubstr("\"existing_relationships\":[]"));
            return EmitLlmResponse(
                R"({"entities":[{"label":"Mochi","category":"pet","source_episode_ids":["ep_001"]}],"relationships":[{"from_label":"user","from_category":"person","predicate":"owns","to_label":"Mochi","to_category":"pet","operation":"APPEND","target_relationship_id":null,"evidence":"EXPLICIT_STATEMENT","source_episode_ids":["ep_001"]}]})",
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
    EXPECT_EQ(result->relationships.front().operation, SemanticRelationshipOperation::Append);
    EXPECT_EQ(result->relationships.front().target_relationship_id, std::nullopt);
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

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsUnknownEvidenceValues) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse(
                R"({"entities":[],"relationships":[{"from_label":"user","from_category":"person","predicate":"owns","to_label":"Mochi","to_category":"pet","operation":"APPEND","target_relationship_id":null,"evidence":"MEDIUM_INFERENCE","source_episode_ids":["ep_001"]}]})",
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
    EXPECT_THAT(result.status().message(), ::testing::HasSubstr("must be one of"));
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsTooManyEntities) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            const std::string entities = RepeatJsonObjectList(
                R"({"label":"Mochi","category":"pet","source_episode_ids":["ep_001"]})", 129U);
            return EmitLlmResponse(
                std::string(R"({"entities":)") + entities + R"(,"relationships":[]})", on_event);
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
    EXPECT_THAT(result.status().message(),
                ::testing::HasSubstr("field 'entities' exceeds max size"));
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsTooManyRelationships) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            const std::string relationships = RepeatJsonObjectList(
                R"({"from_label":"user","from_category":"person","predicate":"owns","to_label":"Mochi","to_category":"pet","operation":"APPEND","target_relationship_id":null,"evidence":"EXPLICIT_STATEMENT","source_episode_ids":["ep_001"]})",
                257U);
            return EmitLlmResponse(
                std::string(R"({"entities":[],"relationships":)") + relationships + "}", on_event);
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
    EXPECT_THAT(result.status().message(),
                ::testing::HasSubstr("field 'relationships' exceeds max size"));
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsTooManySourceEpisodeIdsPerItem) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse(
                R"({"entities":[{"label":"Mochi","category":"pet","source_episode_ids":["ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001","ep_001"]}],"relationships":[]})",
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
    EXPECT_THAT(result.status().message(),
                ::testing::HasSubstr("field 'source_episode_ids' exceeds max size"));
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsUnknownOperationValues) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse(
                R"({"entities":[],"relationships":[{"from_label":"user","from_category":"person","predicate":"owns","to_label":"Mochi","to_category":"pet","operation":"MERGE","target_relationship_id":null,"evidence":"EXPLICIT_STATEMENT","source_episode_ids":["ep_001"]}]})",
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
    EXPECT_THAT(result.status().message(),
                ::testing::HasSubstr("field 'operation' must be one of"));
}

TEST(SleepCycleSemanticExtractorTest, ExtractRejectsSupersedeWithoutTargetRelationshipId) {
    auto llm_client = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*llm_client, StreamResponse(_, _))
        .WillOnce(Invoke([](const LlmRequest& request, const LlmEventCallback& on_event) {
            static_cast<void>(request);
            return EmitLlmResponse(
                R"({"entities":[],"relationships":[{"from_label":"user","from_category":"person","predicate":"lives_in","to_label":"Seattle","to_category":"location","operation":"SUPERSEDE","target_relationship_id":null,"evidence":"EXPLICIT_STATEMENT","source_episode_ids":["ep_001"]}]})",
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
                .existing_relationships =
                    {
                        ExistingSemanticRelationship{
                            .relationship_id = "rel_city",
                            .from_label = "user",
                            .from_category = "person",
                            .predicate = "lives_in",
                            .to_label = "Portland",
                            .to_category = "location",
                            .weight = 10.0,
                            .last_observed_at = Ts("2026-03-01T14:00:00Z"),
                        },
                    },
            });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(),
                ::testing::HasSubstr("must include a non-empty target_relationship_id"));
}

} // namespace
} // namespace isla::server::memory
