#include "server/memory/src/shared/retrieval_ranking.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log.h"

namespace isla::server::memory {
namespace {

constexpr double kMissingEmbeddingScore = -std::numeric_limits<double>::infinity();
constexpr std::uint64_t kMalformedEmbeddingLogFrequency = 100;

enum class SimilarityStatus {
    kOk,
    kMissingEmbedding,
    kEmptyEmbedding,
    kDimensionMismatch,
    kNonFiniteValue,
    kZeroNorm,
};

struct SimilarityResult {
    double score = kMissingEmbeddingScore;
    SimilarityStatus status = SimilarityStatus::kMissingEmbedding;
};

std::string_view SimilarityStatusToString(SimilarityStatus status) {
    switch (status) {
    case SimilarityStatus::kOk:
        return "ok";
    case SimilarityStatus::kMissingEmbedding:
        return "missing_embedding";
    case SimilarityStatus::kEmptyEmbedding:
        return "empty_embedding";
    case SimilarityStatus::kDimensionMismatch:
        return "dimension_mismatch";
    case SimilarityStatus::kNonFiniteValue:
        return "non_finite_value";
    case SimilarityStatus::kZeroNorm:
        return "zero_norm";
    }
    return "unknown";
}

void MaybeLogMalformedEmbedding(std::string_view relationship_id, SimilarityStatus status,
                                std::size_t query_dimensions,
                                std::optional<std::size_t> relationship_dimensions) {
    if (status == SimilarityStatus::kOk || status == SimilarityStatus::kMissingEmbedding) {
        return;
    }

    static std::atomic<std::uint64_t> malformed_embedding_log_count = 0;
    const std::uint64_t count =
        malformed_embedding_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 1 && count % kMalformedEmbeddingLogFrequency != 0) {
        return;
    }

    LOG(WARNING) << "RankEdgesBySimilarity treating malformed relationship embedding as low score"
                 << " relationship_id=" << relationship_id
                 << " reason=" << SimilarityStatusToString(status)
                 << " query_dimensions=" << query_dimensions << " relationship_dimensions="
                 << (relationship_dimensions.has_value() ? std::to_string(*relationship_dimensions)
                                                         : std::string("null"))
                 << " malformed_embedding_log_count=" << count;
}

SimilarityResult ComputeCosineSimilarity(const Embedding& lhs, const Embedding& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kEmptyEmbedding,
        };
    }
    if (lhs.size() != rhs.size()) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kDimensionMismatch,
        };
    }

    double dot_product = 0.0;
    double lhs_norm_squared = 0.0;
    double rhs_norm_squared = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        dot_product += lhs[i] * rhs[i];
        lhs_norm_squared += lhs[i] * lhs[i];
        rhs_norm_squared += rhs[i] * rhs[i];
    }

    if (!std::isfinite(dot_product) || !std::isfinite(lhs_norm_squared) ||
        !std::isfinite(rhs_norm_squared)) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kNonFiniteValue,
        };
    }
    if (lhs_norm_squared <= 0.0 || rhs_norm_squared <= 0.0) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kZeroNorm,
        };
    }

    const double denominator = std::sqrt(lhs_norm_squared) * std::sqrt(rhs_norm_squared);
    if (!std::isfinite(denominator) || denominator <= 0.0) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kZeroNorm,
        };
    }

    const double similarity = dot_product / denominator;
    if (!std::isfinite(similarity)) {
        return SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kNonFiniteValue,
        };
    }
    return SimilarityResult{
        .score = similarity,
        .status = SimilarityStatus::kOk,
    };
}

struct RankedRelationshipIndex {
    std::size_t index = 0;
    double score = kMissingEmbeddingScore;
};

} // namespace

std::vector<Relationship> RankEdgesBySimilarity(const Embedding& query_embedding,
                                                const std::vector<Relationship>& relationships,
                                                std::size_t top_k) {
    if (relationships.empty() || top_k == 0U) {
        return {};
    }

    std::vector<RankedRelationshipIndex> ranked_relationships;
    ranked_relationships.reserve(relationships.size());
    for (std::size_t i = 0; i < relationships.size(); ++i) {
        const Relationship& relationship = relationships[i];
        auto similarity = SimilarityResult{
            .score = kMissingEmbeddingScore,
            .status = SimilarityStatus::kMissingEmbedding,
        };
        if (relationship.embedding.has_value()) {
            similarity = ComputeCosineSimilarity(query_embedding, *relationship.embedding);
            MaybeLogMalformedEmbedding(relationship.relationship_id, similarity.status,
                                       query_embedding.size(), relationship.embedding->size());
        }
        ranked_relationships.push_back(RankedRelationshipIndex{
            .index = i,
            .score = similarity.score,
        });
    }

    const std::size_t limit = std::min(ranked_relationships.size(), top_k);
    std::partial_sort(ranked_relationships.begin(), ranked_relationships.begin() + limit,
                      ranked_relationships.end(),
                      [&](const RankedRelationshipIndex& lhs, const RankedRelationshipIndex& rhs) {
                          if (lhs.score != rhs.score) {
                              return lhs.score > rhs.score;
                          }
                          return relationships[lhs.index].relationship_id <
                                 relationships[rhs.index].relationship_id;
                      });

    std::vector<Relationship> result;
    result.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        result.push_back(relationships[ranked_relationships[i].index]);
    }
    return result;
}

} // namespace isla::server::memory
