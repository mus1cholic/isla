#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "server/memory/src/shared/retrieval_ranking.hpp"

#include <gtest/gtest.h>

namespace isla::server::memory {
namespace {

Relationship MakeRelationship(std::string relationship_id, std::optional<Embedding> embedding) {
    return Relationship{
        .relationship_id = std::move(relationship_id),
        .user_id = "user_001",
        .from_entity_id = "from_entity",
        .predicate = "related_to",
        .to_entity_id = "to_entity",
        .embedding = std::move(embedding),
        .created_at = {},
    };
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityOrdersBySimilarity) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_low", Embedding{ 0.0, 1.0 }),
        MakeRelationship("rel_high", Embedding{ 1.0, 0.0 }),
        MakeRelationship("rel_mid", Embedding{ 0.6, 0.8 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 3);

    ASSERT_EQ(ranked.size(), 3U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_high");
    EXPECT_EQ(ranked[1].relationship_id, "rel_mid");
    EXPECT_EQ(ranked[2].relationship_id, "rel_low");
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityBreaksTiesByRelationshipId) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_b", Embedding{ 1.0, 0.0 }),
        MakeRelationship("rel_a", Embedding{ 1.0, 0.0 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 2);

    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_a");
    EXPECT_EQ(ranked[1].relationship_id, "rel_b");
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityKeepsNullEmbeddingsEligible) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_null", std::nullopt),
        MakeRelationship("rel_ranked", Embedding{ 1.0, 0.0 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 2);

    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_ranked");
    EXPECT_EQ(ranked[1].relationship_id, "rel_null");
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityTruncatesToTopK) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_1", Embedding{ 1.0, 0.0 }),
        MakeRelationship("rel_2", Embedding{ 0.8, 0.2 }),
        MakeRelationship("rel_3", Embedding{ 0.0, 1.0 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 2);

    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_1");
    EXPECT_EQ(ranked[1].relationship_id, "rel_2");
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityTreatsDimensionMismatchAsLowScore) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_mismatch", Embedding{ 1.0, 0.0, 0.0 }),
        MakeRelationship("rel_valid", Embedding{ 1.0, 0.0 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 2);

    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_valid");
    EXPECT_EQ(ranked[1].relationship_id, "rel_mismatch");
}

TEST(RetrievalRankingTest, RankEdgesBySimilarityTreatsNonFiniteEmbeddingAsLowScore) {
    const Embedding query_embedding{ 1.0, 0.0 };
    const std::vector<Relationship> relationships = {
        MakeRelationship("rel_non_finite",
                         Embedding{ std::numeric_limits<double>::infinity(), 0.0 }),
        MakeRelationship("rel_valid", Embedding{ 1.0, 0.0 }),
    };

    const std::vector<Relationship> ranked =
        RankEdgesBySimilarity(query_embedding, relationships, 2);

    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].relationship_id, "rel_valid");
    EXPECT_EQ(ranked[1].relationship_id, "rel_non_finite");
}

} // namespace
} // namespace isla::server::memory
