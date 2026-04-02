#include "isla/server/retrieved_memory_reranker_client_adapter.hpp"

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "reranker_client_mock.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace isla::server {
namespace {

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

TEST(RetrievedMemoryRerankerClientAdapterTest, FactoryRejectsMissingClient) {
    const absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr> reranker =
        CreateClientBackedRetrievedMemoryReranker(nullptr, "bge-reranker-v2-m3");

    ASSERT_FALSE(reranker.ok());
    EXPECT_EQ(reranker.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RetrievedMemoryRerankerClientAdapterTest, FactoryRejectsMissingModel) {
    auto client = std::make_shared<test::MockRerankerClient>();

    const absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr> reranker =
        CreateClientBackedRetrievedMemoryReranker(client, "");

    ASSERT_FALSE(reranker.ok());
    EXPECT_EQ(reranker.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RetrievedMemoryRerankerClientAdapterTest, FactoryPropagatesClientValidationFailure) {
    auto client = std::make_shared<test::MockRerankerClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::InvalidArgumentError("bad config")));

    const absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr> reranker =
        CreateClientBackedRetrievedMemoryReranker(client, "bge-reranker-v2-m3");

    ASSERT_FALSE(reranker.ok());
    EXPECT_EQ(reranker.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(reranker.status().message(), "bad config");
}

TEST(RetrievedMemoryRerankerClientAdapterTest, ScoreDelegatesToClientWithCandidateTexts) {
    auto client = std::make_shared<test::MockRerankerClient>();
    EXPECT_CALL(*client, Validate()).WillOnce(Return(absl::OkStatus()));
    EXPECT_CALL(*client, Rerank(_))
        .WillOnce([](const RerankRequest& request) -> absl::StatusOr<std::vector<double>> {
            EXPECT_EQ(request.model, "bge-reranker-v2-m3");
            EXPECT_EQ(request.query, "Tell me about Mochi");
            EXPECT_THAT(request.candidates, ElementsAre("Airi owns Mochi (weight: 10)",
                                                        "User discussed their cat Mochi"));
            EXPECT_EQ(request.telemetry_context, nullptr);
            return std::vector<double>{ 0.8, 0.6 };
        });

    absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr> reranker =
        CreateClientBackedRetrievedMemoryReranker(client, "bge-reranker-v2-m3");
    ASSERT_TRUE(reranker.ok()) << reranker.status();

    const absl::StatusOr<std::vector<double>> scores = (*reranker)->Score(
        "Tell me about Mochi",
        {
            isla::server::memory::RetrievedMemoryCandidate{
                .kind = isla::server::memory::RetrievedMemoryCandidateKind::KgFact,
                .source_id = "rel_001",
                .candidate_text = "Airi owns Mochi (weight: 10)",
            },
            isla::server::memory::RetrievedMemoryCandidate{
                .kind = isla::server::memory::RetrievedMemoryCandidateKind::Episode,
                .source_id = "lte_001",
                .candidate_text = "User discussed their cat Mochi",
            },
        });

    ASSERT_TRUE(scores.ok()) << scores.status();
    EXPECT_THAT(*scores, ElementsAre(0.8, 0.6));
}

} // namespace
} // namespace isla::server
