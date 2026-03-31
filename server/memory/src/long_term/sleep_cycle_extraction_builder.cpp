#include "sleep_cycle_extraction_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/memory/identifier_utils.hpp"
#include "isla/server/memory/working_memory.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

constexpr std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;
constexpr int kNewSemanticEntityActiveness = 3;
constexpr int kMaxEntityActiveness = 10;
constexpr std::size_t kMaxExistingRelationshipsForSemanticContext = 256;
constexpr double kRelationshipRecencyDecayPerDay = 0.0016;
constexpr double kMinimumEffectiveRelationshipWeightFactor = 0.1;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::uint64_t StableHash64(std::string_view text) {
    std::uint64_t hash = kFnv1aOffsetBasis;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= kFnv1aPrime;
    }
    return hash;
}

std::string BuildLongTermEpisodeId(std::string_view episode_id) {
    return "lte_" + std::string(episode_id);
}

std::string BuildEntityId(std::string_view label, std::string_view category) {
    const std::string normalized_label = NormalizeIdentifierComponent(label);
    const std::string normalized_category = NormalizeIdentifierComponent(category);
    if (normalized_label == "user" &&
        (normalized_category == "person" || normalized_category == "user")) {
        return std::string(kUserEntityId);
    }
    if (normalized_label == "assistant" &&
        (normalized_category == "assistant" || normalized_category == "person")) {
        return std::string(kAssistantEntityId);
    }
    return "entity_" + normalized_label + "_" +
           std::to_string(StableHash64(normalized_category + "|" + normalized_label));
}

std::string CanonicalizePredicate(std::string_view predicate) {
    return NormalizeIdentifierComponent(predicate);
}

std::string BuildRelationshipTripleKey(std::string_view from_entity_id, std::string_view predicate,
                                       std::string_view to_entity_id) {
    return std::string(from_entity_id) + "\n" + CanonicalizePredicate(predicate) + "\n" +
           std::string(to_entity_id);
}

std::string BuildRelationshipId(std::string_view user_id, std::string_view from_entity_id,
                                std::string_view predicate, std::string_view to_entity_id) {
    return "rel_" + std::to_string(StableHash64(
                        std::string(user_id) + "\n" + std::string(from_entity_id) + "\n" +
                        CanonicalizePredicate(predicate) + "\n" + std::string(to_entity_id)));
}

double TranslateEvidenceWeight(SemanticRelationshipEvidence evidence) {
    switch (evidence) {
    case SemanticRelationshipEvidence::ExplicitStatement:
        return 10.0;
    case SemanticRelationshipEvidence::StrongInference:
        return 4.0;
    case SemanticRelationshipEvidence::WeakInference:
        return 1.0;
    }
    return 1.0;
}

double ComputeEffectiveRelationshipWeight(const Relationship& relationship,
                                          Timestamp evaluation_time) {
    if (evaluation_time <= relationship.last_observed_at) {
        return relationship.weight;
    }
    const auto elapsed = evaluation_time - relationship.last_observed_at;
    const double days_since_seen =
        std::chrono::duration<double, std::ratio<86400>>(elapsed).count();
    const double decay_factor = std::max(kMinimumEffectiveRelationshipWeightFactor,
                                         1.0 - (days_since_seen * kRelationshipRecencyDecayPerDay));
    return relationship.weight * decay_factor;
}

std::vector<std::string> SortedVectorFromSet(const std::unordered_set<std::string>& values) {
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

struct AggregatedEntityCandidate {
    std::string entity_id;
    std::string label;
    std::string category;
    std::unordered_set<std::string> source_episode_ids;
    Timestamp earliest_seen_at;
    Timestamp latest_seen_at;
};

struct AggregatedRelationshipObservation {
    std::string from_entity_id;
    std::string predicate;
    std::string to_entity_id;
    SemanticRelationshipOperation operation = SemanticRelationshipOperation::Append;
    std::optional<std::string> target_relationship_id;
    double weight_increment = 0.0;
    int observation_count_increment = 0;
    std::unordered_set<std::string> source_episode_ids;
    Timestamp earliest_seen_at;
    Timestamp latest_seen_at;
};

class SleepCycleExtractionBuilder {
  public:
    explicit SleepCycleExtractionBuilder(const SleepCycleExtractionBuilderInput& input)
        : session_id_(input.session_id), user_id_(input.user_id),
          mid_term_episodes_(input.mid_term_episodes), store_(input.store),
          semantic_extractor_(input.semantic_extractor) {}

    [[nodiscard]] absl::StatusOr<SleepCycleExtractionResult> Build();

  private:
    void BuildLongTermEpisodes(SleepCycleExtractionResult& result);
    [[nodiscard]] absl::Status BuildSemanticWrites(SleepCycleExtractionResult& result);
    [[nodiscard]] absl::StatusOr<SleepCycleSemanticExtractionResult> ExtractSemanticCandidates();
    [[nodiscard]] std::unordered_set<std::string>
    CollectRelevantEntityIds(const SleepCycleSemanticExtractionResult& semantic_result) const;
    [[nodiscard]] absl::StatusOr<std::optional<Entity>>
    LoadContextEntity(const std::string& entity_id);
    [[nodiscard]] absl::StatusOr<std::vector<ExistingSemanticRelationship>>
    LoadExistingRelationshipContext(const std::unordered_set<std::string>& relevant_entity_ids);
    [[nodiscard]] absl::StatusOr<std::string>
    ObserveEntity(std::string_view label, std::string_view category,
                  const std::vector<std::string>& source_episode_ids);
    [[nodiscard]] absl::Status
    AggregateSemanticObservations(const SleepCycleSemanticExtractionResult& semantic_result);
    [[nodiscard]] absl::StatusOr<std::optional<Entity>>
    LoadExistingEntity(const std::string& entity_id);
    [[nodiscard]] absl::Status BuildEntityWrites(SleepCycleExtractionResult& result);
    [[nodiscard]] absl::StatusOr<std::vector<Relationship>>
    LoadExistingRelationships(const std::string& entity_id);
    [[nodiscard]] absl::Status AppendRelationshipWrite(SleepCycleExtractionResult& result,
                                                       Relationship relationship);
    [[nodiscard]] absl::Status BuildRelationshipWrites(SleepCycleExtractionResult& result);
    void BuildEpisodeEntityLinks(SleepCycleExtractionResult& result);

    std::string_view session_id_;
    std::string_view user_id_;
    const std::vector<Episode>* mid_term_episodes_;
    MemoryStore* store_;
    SleepCycleSemanticExtractor* semantic_extractor_;
    std::unordered_map<std::string, const Episode*> episodes_by_id_;
    std::unordered_map<std::string, std::optional<Entity>> context_entities_by_id_;
    std::unordered_map<std::string, std::vector<Relationship>> existing_relationships_by_entity_;
    std::unordered_map<std::string, Relationship> existing_relationships_by_id_;
    std::unordered_map<std::string, AggregatedEntityCandidate> aggregated_entities_;
    std::unordered_map<std::string, AggregatedRelationshipObservation> aggregated_relationships_;
    std::unordered_map<std::string, std::unordered_set<std::string>> entity_ids_by_lte_id_;
    std::unordered_map<std::string, std::optional<Entity>> existing_entities_by_id_;
    std::unordered_set<std::string> missing_entity_ids_;
    std::unordered_set<std::string> emitted_relationship_ids_;
};

absl::StatusOr<SleepCycleExtractionResult> SleepCycleExtractionBuilder::Build() {
    if (user_id_.empty()) {
        return invalid_argument("sleep-cycle extraction requires a non-empty user_id");
    }
    if (mid_term_episodes_ == nullptr) {
        return invalid_argument("sleep-cycle extraction requires mid_term_episodes");
    }
    if (semantic_extractor_ != nullptr && store_ == nullptr) {
        return invalid_argument(
            "sleep-cycle extraction requires a store when semantic extraction is enabled");
    }

    SleepCycleExtractionResult result;
    BuildLongTermEpisodes(result);
    if (semantic_extractor_ != nullptr) {
        absl::Status status = BuildSemanticWrites(result);
        if (!status.ok()) {
            return status;
        }
    }

    if (absl::Status status = ValidateSleepCycleExtractionResult(result); !status.ok()) {
        return status;
    }
    return result;
}

void SleepCycleExtractionBuilder::BuildLongTermEpisodes(SleepCycleExtractionResult& result) {
    result.long_term_episodes.reserve(mid_term_episodes_->size());
    episodes_by_id_.reserve(mid_term_episodes_->size());

    for (const Episode& episode : *mid_term_episodes_) {
        episodes_by_id_.emplace(episode.episode_id, &episode);
        result.long_term_episodes.push_back(LongTermEpisodeWrite{
            .episode =
                LongTermEpisode{
                    .lte_id = BuildLongTermEpisodeId(episode.episode_id),
                    .user_id = std::string(user_id_),
                    .summary_full = episode.tier1_detail,
                    .summary_compressed = episode.tier2_summary,
                    .keywords = episode.tier3_keywords,
                    .embedding = episode.embedding.empty()
                                     ? std::nullopt
                                     : std::optional<Embedding>(episode.embedding),
                    .complexity = episode.salience,
                    .created_at = episode.created_at,
                    .original_episode_ids = { episode.episode_id },
                },
        });
    }
}

absl::Status SleepCycleExtractionBuilder::BuildSemanticWrites(SleepCycleExtractionResult& result) {
    absl::StatusOr<SleepCycleSemanticExtractionResult> semantic_result =
        ExtractSemanticCandidates();
    if (!semantic_result.ok()) {
        return semantic_result.status();
    }

    absl::Status aggregation_status = AggregateSemanticObservations(*semantic_result);
    if (!aggregation_status.ok()) {
        return aggregation_status;
    }

    absl::Status entity_status = BuildEntityWrites(result);
    if (!entity_status.ok()) {
        return entity_status;
    }

    absl::Status relationship_status = BuildRelationshipWrites(result);
    if (!relationship_status.ok()) {
        return relationship_status;
    }

    BuildEpisodeEntityLinks(result);
    return absl::OkStatus();
}

absl::StatusOr<SleepCycleSemanticExtractionResult>
SleepCycleExtractionBuilder::ExtractSemanticCandidates() {
    SleepCycleSemanticExtractionRequest semantic_request{
        .user_id = std::string(user_id_),
        .mid_term_episodes = *mid_term_episodes_,
        .existing_relationships = {},
    };
    absl::StatusOr<SleepCycleSemanticExtractionResult> preliminary_semantic_result =
        semantic_extractor_->Extract(semantic_request);
    if (!preliminary_semantic_result.ok()) {
        return preliminary_semantic_result.status();
    }
    SleepCycleSemanticExtractionResult semantic_result = *preliminary_semantic_result;

    const std::unordered_set<std::string> relevant_entity_ids =
        CollectRelevantEntityIds(semantic_result);
    if (relevant_entity_ids.empty()) {
        return semantic_result;
    }

    absl::StatusOr<std::vector<ExistingSemanticRelationship>> existing_relationships =
        LoadExistingRelationshipContext(relevant_entity_ids);
    if (!existing_relationships.ok()) {
        return existing_relationships.status();
    }
    if (existing_relationships->empty()) {
        return semantic_result;
    }

    semantic_request.existing_relationships = std::move(*existing_relationships);
    absl::StatusOr<SleepCycleSemanticExtractionResult> contextual_semantic_result =
        semantic_extractor_->Extract(semantic_request);
    if (!contextual_semantic_result.ok()) {
        return contextual_semantic_result.status();
    }
    return std::move(*contextual_semantic_result);
}

std::unordered_set<std::string> SleepCycleExtractionBuilder::CollectRelevantEntityIds(
    const SleepCycleSemanticExtractionResult& semantic_result) const {
    std::unordered_set<std::string> entity_ids;
    for (const SemanticEntityCandidate& entity : semantic_result.entities) {
        entity_ids.insert(BuildEntityId(entity.label, entity.category));
    }
    for (const SemanticRelationshipObservation& relationship : semantic_result.relationships) {
        entity_ids.insert(BuildEntityId(relationship.from_label, relationship.from_category));
        entity_ids.insert(BuildEntityId(relationship.to_label, relationship.to_category));
    }
    return entity_ids;
}

absl::StatusOr<std::optional<Entity>>
SleepCycleExtractionBuilder::LoadContextEntity(const std::string& entity_id) {
    if (const auto it = context_entities_by_id_.find(entity_id);
        it != context_entities_by_id_.end()) {
        return it->second;
    }

    const absl::StatusOr<std::optional<Entity>> entity = store_->GetEntity(entity_id);
    if (!entity.ok()) {
        if (absl::IsUnimplemented(entity.status())) {
            context_entities_by_id_.emplace(entity_id, std::nullopt);
            return std::optional<Entity>{};
        }
        return entity.status();
    }
    context_entities_by_id_.emplace(entity_id, *entity);
    return *entity;
}

absl::StatusOr<std::vector<ExistingSemanticRelationship>>
SleepCycleExtractionBuilder::LoadExistingRelationshipContext(
    const std::unordered_set<std::string>& relevant_entity_ids) {
    std::unordered_set<std::string> seen_relationship_ids;
    std::vector<ExistingSemanticRelationship> existing_relationships;

    for (const std::string& entity_id : relevant_entity_ids) {
        const absl::StatusOr<std::vector<Relationship>> entity_relationships =
            store_->ListRelationshipsForEntity(entity_id);
        if (!entity_relationships.ok()) {
            if (absl::IsUnimplemented(entity_relationships.status())) {
                existing_relationships_by_entity_.emplace(entity_id, std::vector<Relationship>{});
                continue;
            }
            return entity_relationships.status();
        }
        existing_relationships_by_entity_.emplace(entity_id, *entity_relationships);
        for (const Relationship& relationship : *entity_relationships) {
            existing_relationships_by_id_.emplace(relationship.relationship_id, relationship);
            if (relationship.is_archived ||
                !seen_relationship_ids.insert(relationship.relationship_id).second) {
                continue;
            }
            const absl::StatusOr<std::optional<Entity>> from_entity =
                LoadContextEntity(relationship.from_entity_id);
            if (!from_entity.ok()) {
                return from_entity.status();
            }
            if (!from_entity->has_value()) {
                continue;
            }
            const absl::StatusOr<std::optional<Entity>> to_entity =
                LoadContextEntity(relationship.to_entity_id);
            if (!to_entity.ok()) {
                return to_entity.status();
            }
            if (!to_entity->has_value()) {
                continue;
            }
            existing_relationships.push_back(ExistingSemanticRelationship{
                .relationship_id = relationship.relationship_id,
                .from_label = from_entity->value().label,
                .from_category = from_entity->value().category,
                .predicate = relationship.predicate,
                .to_label = to_entity->value().label,
                .to_category = to_entity->value().category,
                .weight = relationship.weight,
                .last_observed_at = relationship.last_observed_at,
            });
        }
    }

    std::sort(existing_relationships.begin(), existing_relationships.end(),
              [](const ExistingSemanticRelationship& lhs, const ExistingSemanticRelationship& rhs) {
                  if (lhs.weight != rhs.weight) {
                      return lhs.weight > rhs.weight;
                  }
                  if (lhs.last_observed_at != rhs.last_observed_at) {
                      return lhs.last_observed_at > rhs.last_observed_at;
                  }
                  return lhs.relationship_id < rhs.relationship_id;
              });
    if (existing_relationships.size() > kMaxExistingRelationshipsForSemanticContext) {
        LOG(WARNING)
            << "SleepCycleExtractionBuilder truncated semantic extraction relationship context"
            << " session_id=" << SanitizeForLog(session_id_)
            << " original_count=" << existing_relationships.size()
            << " truncated_count=" << kMaxExistingRelationshipsForSemanticContext;
        existing_relationships.resize(kMaxExistingRelationshipsForSemanticContext);
    }
    return existing_relationships;
}

absl::StatusOr<std::string>
SleepCycleExtractionBuilder::ObserveEntity(std::string_view label, std::string_view category,
                                           const std::vector<std::string>& source_episode_ids) {
    if (source_episode_ids.empty()) {
        return invalid_argument("sleep-cycle semantic extraction requires entity "
                                "source_episode_ids");
    }

    std::string entity_id = BuildEntityId(label, category);
    auto entity_it = aggregated_entities_.find(entity_id);

    for (const std::string& source_episode_id : source_episode_ids) {
        const auto episode_it = episodes_by_id_.find(source_episode_id);
        if (episode_it == episodes_by_id_.end()) {
            return invalid_argument(
                "sleep-cycle semantic extraction referenced an unknown source episode");
        }
        const Timestamp seen_at = episode_it->second->created_at;
        if (entity_it == aggregated_entities_.end()) {
            entity_it = aggregated_entities_
                            .try_emplace(entity_id,
                                         AggregatedEntityCandidate{
                                             .entity_id = entity_id,
                                             .label = std::string(label),
                                             .category = std::string(category),
                                             .source_episode_ids = {},
                                             .earliest_seen_at = seen_at,
                                             .latest_seen_at = seen_at,
                                         })
                            .first;
        }
        entity_it->second.source_episode_ids.insert(source_episode_id);
        entity_it->second.earliest_seen_at = std::min(entity_it->second.earliest_seen_at, seen_at);
        entity_it->second.latest_seen_at = std::max(entity_it->second.latest_seen_at, seen_at);
        entity_ids_by_lte_id_[BuildLongTermEpisodeId(source_episode_id)].insert(entity_id);
    }

    return entity_id;
}

absl::Status SleepCycleExtractionBuilder::AggregateSemanticObservations(
    const SleepCycleSemanticExtractionResult& semantic_result) {
    for (const SemanticEntityCandidate& entity : semantic_result.entities) {
        absl::StatusOr<std::string> observed_entity_id =
            ObserveEntity(entity.label, entity.category, entity.source_episode_ids);
        if (!observed_entity_id.ok()) {
            return observed_entity_id.status();
        }
    }

    for (const SemanticRelationshipObservation& relationship : semantic_result.relationships) {
        absl::StatusOr<std::string> from_entity_id = ObserveEntity(
            relationship.from_label, relationship.from_category, relationship.source_episode_ids);
        if (!from_entity_id.ok()) {
            return from_entity_id.status();
        }
        absl::StatusOr<std::string> to_entity_id = ObserveEntity(
            relationship.to_label, relationship.to_category, relationship.source_episode_ids);
        if (!to_entity_id.ok()) {
            return to_entity_id.status();
        }
        if (relationship.source_episode_ids.empty()) {
            return invalid_argument(
                "sleep-cycle semantic extraction requires relationship source_episode_ids");
        }

        const std::string predicate = CanonicalizePredicate(relationship.predicate);
        const std::string relationship_key =
            BuildRelationshipTripleKey(*from_entity_id, predicate, *to_entity_id);
        auto relationship_it = aggregated_relationships_.find(relationship_key);
        if (relationship_it != aggregated_relationships_.end() &&
            (relationship_it->second.operation != relationship.operation ||
             relationship_it->second.target_relationship_id !=
                 relationship.target_relationship_id)) {
            return invalid_argument("sleep-cycle semantic extraction produced conflicting "
                                    "operations for the same relationship triple");
        }

        for (const std::string& source_episode_id : relationship.source_episode_ids) {
            const auto episode_it = episodes_by_id_.find(source_episode_id);
            if (episode_it == episodes_by_id_.end()) {
                return invalid_argument(
                    "sleep-cycle semantic extraction referenced an unknown source episode");
            }
            const Timestamp seen_at = episode_it->second->created_at;
            if (relationship_it == aggregated_relationships_.end()) {
                relationship_it = aggregated_relationships_
                                      .try_emplace(relationship_key,
                                                   AggregatedRelationshipObservation{
                                                       .from_entity_id = *from_entity_id,
                                                       .predicate = predicate,
                                                       .to_entity_id = *to_entity_id,
                                                       .operation = relationship.operation,
                                                       .target_relationship_id =
                                                           relationship.target_relationship_id,
                                                       .weight_increment = 0.0,
                                                       .observation_count_increment = 0,
                                                       .source_episode_ids = {},
                                                       .earliest_seen_at = seen_at,
                                                       .latest_seen_at = seen_at,
                                                   })
                                      .first;
            }
            relationship_it->second.source_episode_ids.insert(source_episode_id);
            relationship_it->second.earliest_seen_at =
                std::min(relationship_it->second.earliest_seen_at, seen_at);
            relationship_it->second.latest_seen_at =
                std::max(relationship_it->second.latest_seen_at, seen_at);
        }
        relationship_it->second.weight_increment += TranslateEvidenceWeight(relationship.evidence);
        relationship_it->second.observation_count_increment += 1;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<Entity>>
SleepCycleExtractionBuilder::LoadExistingEntity(const std::string& entity_id) {
    if (const auto it = existing_entities_by_id_.find(entity_id);
        it != existing_entities_by_id_.end()) {
        return it->second;
    }
    if (missing_entity_ids_.contains(entity_id)) {
        return std::optional<Entity>{};
    }

    const absl::StatusOr<std::optional<Entity>> existing_entity = store_->GetEntity(entity_id);
    if (!existing_entity.ok()) {
        if (absl::IsUnimplemented(existing_entity.status())) {
            missing_entity_ids_.insert(entity_id);
            return std::optional<Entity>{};
        }
        return existing_entity.status();
    }
    if (existing_entity->has_value()) {
        existing_entities_by_id_.emplace(entity_id, *existing_entity);
    } else {
        missing_entity_ids_.insert(entity_id);
    }
    return *existing_entity;
}

absl::Status SleepCycleExtractionBuilder::BuildEntityWrites(SleepCycleExtractionResult& result) {
    result.entities.reserve(aggregated_entities_.size());
    for (const auto& [entity_id, aggregated_entity] : aggregated_entities_) {
        absl::StatusOr<std::optional<Entity>> existing_entity = LoadExistingEntity(entity_id);
        if (!existing_entity.ok()) {
            return existing_entity.status();
        }

        Entity entity;
        if (existing_entity->has_value()) {
            if (existing_entity->value().user_id != user_id_) {
                return invalid_argument("sleep-cycle semantic extraction resolved an entity owned "
                                        "by a different user");
            }
            entity = **existing_entity;
            entity.activeness = std::min(kMaxEntityActiveness, entity.activeness + 1);
            entity.updated_at = std::max(entity.updated_at, aggregated_entity.latest_seen_at);
        } else {
            entity = Entity{
                .entity_id = entity_id,
                .user_id = std::string(user_id_),
                .label = aggregated_entity.label,
                .category = aggregated_entity.category,
                .activeness = kNewSemanticEntityActiveness,
                .created_at = aggregated_entity.earliest_seen_at,
                .updated_at = aggregated_entity.latest_seen_at,
            };
        }

        result.entities.push_back(EntityWrite{ .entity = std::move(entity) });
    }

    std::sort(result.entities.begin(), result.entities.end(),
              [](const EntityWrite& lhs, const EntityWrite& rhs) {
                  return lhs.entity.entity_id < rhs.entity.entity_id;
              });
    return absl::OkStatus();
}

absl::StatusOr<std::vector<Relationship>>
SleepCycleExtractionBuilder::LoadExistingRelationships(const std::string& entity_id) {
    if (const auto it = existing_relationships_by_entity_.find(entity_id);
        it != existing_relationships_by_entity_.end()) {
        return it->second;
    }

    const absl::StatusOr<std::vector<Relationship>> existing_relationships =
        store_->ListRelationshipsForEntity(entity_id);
    if (!existing_relationships.ok()) {
        if (absl::IsUnimplemented(existing_relationships.status())) {
            existing_relationships_by_entity_.emplace(entity_id, std::vector<Relationship>{});
            return std::vector<Relationship>{};
        }
        return existing_relationships.status();
    }
    for (const Relationship& relationship : *existing_relationships) {
        existing_relationships_by_id_.emplace(relationship.relationship_id, relationship);
    }
    existing_relationships_by_entity_.emplace(entity_id, *existing_relationships);
    return *existing_relationships;
}

absl::Status
SleepCycleExtractionBuilder::AppendRelationshipWrite(SleepCycleExtractionResult& result,
                                                     Relationship relationship) {
    if (!emitted_relationship_ids_.insert(relationship.relationship_id).second) {
        return invalid_argument(
            "sleep-cycle extraction emitted multiple writes for the same relationship_id");
    }
    result.relationships.push_back(RelationshipWrite{ .relationship = std::move(relationship) });
    return absl::OkStatus();
}

absl::Status
SleepCycleExtractionBuilder::BuildRelationshipWrites(SleepCycleExtractionResult& result) {
    result.relationships.reserve(aggregated_relationships_.size());

    for (const auto& [relationship_key, aggregated_relationship] : aggregated_relationships_) {
        static_cast<void>(relationship_key);
        absl::StatusOr<std::vector<Relationship>> existing_relationships =
            LoadExistingRelationships(aggregated_relationship.from_entity_id);
        if (!existing_relationships.ok()) {
            return existing_relationships.status();
        }

        const Relationship* matched_relationship = nullptr;
        for (const Relationship& relationship : *existing_relationships) {
            if (relationship.is_archived || relationship.user_id != user_id_ ||
                relationship.from_entity_id != aggregated_relationship.from_entity_id ||
                relationship.to_entity_id != aggregated_relationship.to_entity_id ||
                CanonicalizePredicate(relationship.predicate) !=
                    aggregated_relationship.predicate) {
                continue;
            }
            matched_relationship = &relationship;
            break;
        }

        const Relationship* target_relationship = nullptr;
        if (aggregated_relationship.target_relationship_id.has_value()) {
            const auto target_it =
                existing_relationships_by_id_.find(*aggregated_relationship.target_relationship_id);
            if (target_it == existing_relationships_by_id_.end()) {
                return invalid_argument("sleep-cycle semantic extraction referenced an unknown "
                                        "target relationship");
            }
            target_relationship = &target_it->second;
            if (target_relationship->user_id != user_id_) {
                return invalid_argument("sleep-cycle semantic extraction referenced a target "
                                        "relationship owned by a different user");
            }
            if (target_relationship->is_archived) {
                return invalid_argument("sleep-cycle semantic extraction referenced an archived "
                                        "target relationship");
            }
        }

        const auto build_new_relationship = [&]() {
            return Relationship{
                .relationship_id = BuildRelationshipId(
                    user_id_, aggregated_relationship.from_entity_id,
                    aggregated_relationship.predicate, aggregated_relationship.to_entity_id),
                .user_id = std::string(user_id_),
                .from_entity_id = aggregated_relationship.from_entity_id,
                .predicate = aggregated_relationship.predicate,
                .to_entity_id = aggregated_relationship.to_entity_id,
                .weight = aggregated_relationship.weight_increment,
                .observation_count = aggregated_relationship.observation_count_increment,
                .last_observed_at = aggregated_relationship.latest_seen_at,
                .source_episode_ids =
                    SortedVectorFromSet(aggregated_relationship.source_episode_ids),
                .created_at = aggregated_relationship.earliest_seen_at,
            };
        };

        switch (aggregated_relationship.operation) {
        case SemanticRelationshipOperation::Append: {
            Relationship relationship =
                matched_relationship != nullptr ? *matched_relationship : build_new_relationship();
            if (matched_relationship != nullptr) {
                relationship.weight += aggregated_relationship.weight_increment;
                relationship.observation_count +=
                    aggregated_relationship.observation_count_increment;
                relationship.last_observed_at =
                    std::max(relationship.last_observed_at, aggregated_relationship.latest_seen_at);
                std::unordered_set<std::string> source_episode_ids(
                    relationship.source_episode_ids.begin(), relationship.source_episode_ids.end());
                source_episode_ids.insert(aggregated_relationship.source_episode_ids.begin(),
                                          aggregated_relationship.source_episode_ids.end());
                relationship.source_episode_ids = SortedVectorFromSet(source_episode_ids);
            }
            if (absl::Status status = AppendRelationshipWrite(result, std::move(relationship));
                !status.ok()) {
                return status;
            }
            break;
        }
        case SemanticRelationshipOperation::Strengthen: {
            if (target_relationship == nullptr) {
                return invalid_argument(
                    "sleep-cycle semantic extraction STRENGTHEN relationship is missing its "
                    "target");
            }
            if (target_relationship->from_entity_id != aggregated_relationship.from_entity_id ||
                target_relationship->to_entity_id != aggregated_relationship.to_entity_id ||
                CanonicalizePredicate(target_relationship->predicate) !=
                    aggregated_relationship.predicate) {
                return invalid_argument("sleep-cycle semantic extraction STRENGTHEN target does "
                                        "not match the observed relationship triple");
            }
            Relationship relationship = *target_relationship;
            relationship.weight += aggregated_relationship.weight_increment;
            relationship.observation_count += aggregated_relationship.observation_count_increment;
            relationship.last_observed_at =
                std::max(relationship.last_observed_at, aggregated_relationship.latest_seen_at);
            std::unordered_set<std::string> source_episode_ids(
                relationship.source_episode_ids.begin(), relationship.source_episode_ids.end());
            source_episode_ids.insert(aggregated_relationship.source_episode_ids.begin(),
                                      aggregated_relationship.source_episode_ids.end());
            relationship.source_episode_ids = SortedVectorFromSet(source_episode_ids);
            if (absl::Status status = AppendRelationshipWrite(result, std::move(relationship));
                !status.ok()) {
                return status;
            }
            break;
        }
        case SemanticRelationshipOperation::Supersede: {
            if (target_relationship == nullptr) {
                return invalid_argument(
                    "sleep-cycle semantic extraction SUPERSEDE relationship is missing its "
                    "target");
            }
            if (target_relationship->from_entity_id != aggregated_relationship.from_entity_id ||
                CanonicalizePredicate(target_relationship->predicate) !=
                    aggregated_relationship.predicate) {
                return invalid_argument("sleep-cycle semantic extraction SUPERSEDE target must be "
                                        "comparable to the observed relationship triple (same "
                                        "source entity and predicate)");
            }
            if (target_relationship->to_entity_id == aggregated_relationship.to_entity_id) {
                return invalid_argument("sleep-cycle semantic extraction SUPERSEDE target must "
                                        "differ from the observed relationship triple");
            }

            const double effective_old_weight = ComputeEffectiveRelationshipWeight(
                *target_relationship, aggregated_relationship.latest_seen_at);
            if (aggregated_relationship.weight_increment > effective_old_weight) {
                Relationship new_relationship = build_new_relationship();
                Relationship archived_relationship = *target_relationship;
                archived_relationship.is_archived = true;
                archived_relationship.archived_at = aggregated_relationship.latest_seen_at;
                archived_relationship.superseded_by = new_relationship.relationship_id;
                if (absl::Status status =
                        AppendRelationshipWrite(result, std::move(archived_relationship));
                    !status.ok()) {
                    return status;
                }
                if (absl::Status status =
                        AppendRelationshipWrite(result, std::move(new_relationship));
                    !status.ok()) {
                    return status;
                }
            } else {
                Relationship surviving_relationship = *target_relationship;
                surviving_relationship.weight = std::max(
                    0.0, surviving_relationship.weight - aggregated_relationship.weight_increment);
                if (absl::Status status =
                        AppendRelationshipWrite(result, std::move(surviving_relationship));
                    !status.ok()) {
                    return status;
                }
            }
            break;
        }
        }
    }

    std::sort(result.relationships.begin(), result.relationships.end(),
              [](const RelationshipWrite& lhs, const RelationshipWrite& rhs) {
                  return lhs.relationship.relationship_id < rhs.relationship.relationship_id;
              });
    return absl::OkStatus();
}

void SleepCycleExtractionBuilder::BuildEpisodeEntityLinks(SleepCycleExtractionResult& result) {
    result.long_term_episode_entity_links.reserve(entity_ids_by_lte_id_.size());
    for (auto& [lte_id, entity_ids] : entity_ids_by_lte_id_) {
        if (entity_ids.empty()) {
            continue;
        }
        result.long_term_episode_entity_links.push_back(LongTermEpisodeEntityLink{
            .lte_id = lte_id,
            .entity_ids = SortedVectorFromSet(entity_ids),
        });
    }

    std::sort(result.long_term_episode_entity_links.begin(),
              result.long_term_episode_entity_links.end(),
              [](const LongTermEpisodeEntityLink& lhs, const LongTermEpisodeEntityLink& rhs) {
                  return lhs.lte_id < rhs.lte_id;
              });
}

} // namespace

absl::StatusOr<SleepCycleExtractionResult>
BuildSleepCycleExtractionResult(const SleepCycleExtractionBuilderInput& input) {
    return SleepCycleExtractionBuilder(input).Build();
}

} // namespace isla::server::memory
