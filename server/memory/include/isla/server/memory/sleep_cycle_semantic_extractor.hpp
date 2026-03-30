#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

enum class SemanticRelationshipEvidence {
    ExplicitStatement,
    StrongInference,
    WeakInference,
};

struct SemanticEntityCandidate {
    std::string label;
    std::string category;
    std::vector<std::string> source_episode_ids;
};

struct SemanticRelationshipObservation {
    std::string from_label;
    std::string from_category;
    std::string predicate;
    std::string to_label;
    std::string to_category;
    SemanticRelationshipEvidence evidence = SemanticRelationshipEvidence::WeakInference;
    std::vector<std::string> source_episode_ids;
};

struct SleepCycleSemanticExtractionRequest {
    std::string user_id;
    std::vector<Episode> mid_term_episodes;
};

struct SleepCycleSemanticExtractionResult {
    std::vector<SemanticEntityCandidate> entities;
    std::vector<SemanticRelationshipObservation> relationships;
};

class SleepCycleSemanticExtractor {
  public:
    virtual ~SleepCycleSemanticExtractor() = default;

    // Extracts semantic entities and relationship observations from the current sleep-cycle
    // episode set. The backend is responsible for translating observations into KG writes and
    // evidence accumulation math.
    [[nodiscard]] virtual absl::StatusOr<SleepCycleSemanticExtractionResult>
    Extract(const SleepCycleSemanticExtractionRequest& request) = 0;
};

using SleepCycleSemanticExtractorPtr = std::shared_ptr<SleepCycleSemanticExtractor>;

[[nodiscard]] absl::StatusOr<SleepCycleSemanticExtractorPtr> CreateLlmSleepCycleSemanticExtractor(
    std::shared_ptr<const isla::server::LlmClient> llm_client, std::string model,
    isla::server::LlmReasoningEffort reasoning_effort = isla::server::LlmReasoningEffort::kNone);

NLOHMANN_JSON_SERIALIZE_ENUM(
    SemanticRelationshipEvidence,
    {
        { SemanticRelationshipEvidence::ExplicitStatement, "EXPLICIT_STATEMENT" },
        { SemanticRelationshipEvidence::StrongInference, "STRONG_INFERENCE" },
        { SemanticRelationshipEvidence::WeakInference, "WEAK_INFERENCE" },
    })

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SemanticEntityCandidate, label, category,
                                                source_episode_ids)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SemanticRelationshipObservation, from_label,
                                                from_category, predicate, to_label, to_category,
                                                evidence, source_episode_ids)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SleepCycleSemanticExtractionRequest, user_id,
                                                mid_term_episodes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SleepCycleSemanticExtractionResult, entities,
                                                relationships)

} // namespace isla::server::memory
