#include "isla/server/memory/memory_orchestrator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
#include "isla/server/ai_gateway_telemetry.hpp"
#include "isla/server/embedding_client.hpp"
#include "isla/server/memory/identifier_utils.hpp"
#include "isla/server/memory/working_memory.hpp"
#include "server/memory/src/shared/retrieval_ranking.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::SanitizeForLog;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

bool ContainsNormalizedTerm(std::string_view normalized_text, std::string_view normalized_term) {
    if (normalized_text.empty() || normalized_term.empty()) {
        return false;
    }

    std::size_t position = normalized_text.find(normalized_term);
    while (position != std::string_view::npos) {
        const bool has_left_boundary = position == 0 || normalized_text[position - 1] == '_';
        const std::size_t end = position + normalized_term.size();
        const bool has_right_boundary =
            end == normalized_text.size() || normalized_text[end] == '_';
        if (has_left_boundary && has_right_boundary) {
            return true;
        }
        position = normalized_text.find(normalized_term, position + 1U);
    }
    return false;
}

bool QueryReferencesCoreEntity(std::string_view entity_id, std::string_view normalized_query) {
    if (entity_id == kUserEntityId) {
        return ContainsNormalizedTerm(normalized_query, "i") ||
               ContainsNormalizedTerm(normalized_query, "me") ||
               ContainsNormalizedTerm(normalized_query, "my") ||
               ContainsNormalizedTerm(normalized_query, "mine");
    }
    if (entity_id == kAssistantEntityId) {
        return ContainsNormalizedTerm(normalized_query, "you") ||
               ContainsNormalizedTerm(normalized_query, "your") ||
               ContainsNormalizedTerm(normalized_query, "yours");
    }
    return false;
}

struct RetrievedRelationshipCandidate {
    std::string relationship_id;
    std::string from_text;
    std::string predicate;
    std::string to_text;
    double weight = 0.0;
};

struct RetrievedEpisodeCandidate {
    std::string lte_id;
    std::string summary_compressed;
    Timestamp created_at;
};

struct RankedRetrievedMemoryCandidate {
    RetrievedMemoryCandidate candidate;
    std::optional<RetrievedRelationshipCandidate> relationship;
    std::optional<RetrievedEpisodeCandidate> episode;
    double score = 0.0;
};

std::string
RenderRetrievedRelationshipCandidateText(const RetrievedRelationshipCandidate& relationship);

std::string RenderRetrievedEpisodeCandidateText(const RetrievedEpisodeCandidate& episode);

double SanitizeRetrievedMemoryRerankerScore(double score) {
    return std::isfinite(score) ? score : -std::numeric_limits<double>::infinity();
}

absl::StatusOr<std::unordered_map<std::string, std::optional<Entity>>>
LoadEntityMapForRetrievedMemory(MemoryStore* store, const std::vector<std::string>& entity_ids) {
    std::unordered_map<std::string, std::optional<Entity>> entities_by_id;
    if (entity_ids.empty()) {
        return entities_by_id;
    }

    const absl::StatusOr<std::vector<Entity>> entities = store->ListEntitiesByIds(entity_ids);
    if (entities.ok()) {
        entities_by_id.reserve(entities->size());
        for (const Entity& entity : *entities) {
            entities_by_id.emplace(entity.entity_id, entity);
        }
        return entities_by_id;
    }
    if (!absl::IsUnimplemented(entities.status())) {
        return entities.status();
    }

    entities_by_id.reserve(entity_ids.size());
    for (const std::string& entity_id : entity_ids) {
        const absl::StatusOr<std::optional<Entity>> entity = store->GetEntity(entity_id);
        if (!entity.ok()) {
            if (absl::IsUnimplemented(entity.status())) {
                entities_by_id.emplace(entity_id, std::nullopt);
                continue;
            }
            return entity.status();
        }
        entities_by_id.emplace(entity_id, *entity);
    }
    return entities_by_id;
}

std::vector<std::string> SelectSeedEntityIdsForRetrievedMemory(
    const Message& user_message, const std::vector<std::string>& cache_entity_ids,
    const std::unordered_map<std::string, std::optional<Entity>>& cache_entities_by_id) {
    const std::string normalized_query = NormalizeIdentifierComponent(user_message.content);
    if (normalized_query.empty()) {
        return {};
    }

    bool has_any_usable_noncore_label = false;
    std::vector<std::string> seed_entity_ids;
    seed_entity_ids.reserve(cache_entity_ids.size());
    for (const std::string& entity_id : cache_entity_ids) {
        if (QueryReferencesCoreEntity(entity_id, normalized_query)) {
            seed_entity_ids.push_back(entity_id);
            continue;
        }

        const auto entity_it = cache_entities_by_id.find(entity_id);
        if (entity_it == cache_entities_by_id.end() || !entity_it->second.has_value()) {
            continue;
        }
        const std::string normalized_label = NormalizeIdentifierComponent(entity_it->second->label);
        if (!normalized_label.empty()) {
            has_any_usable_noncore_label = true;
        }
        if (ContainsNormalizedTerm(normalized_query, normalized_label)) {
            seed_entity_ids.push_back(entity_id);
        }
    }
    if (seed_entity_ids.empty() && !has_any_usable_noncore_label) {
        return cache_entity_ids;
    }
    return seed_entity_ids;
}

std::string RenderRetrievedMemory(const std::vector<RetrievedRelationshipCandidate>& relationships,
                                  const std::vector<RetrievedEpisodeCandidate>& episodes) {
    std::string output;
    if (!relationships.empty()) {
        output.append("KG Facts:\n");
        for (const RetrievedRelationshipCandidate& relationship : relationships) {
            output.append("- ");
            output.append(RenderRetrievedRelationshipCandidateText(relationship));
            output.push_back('\n');
        }
    }
    if (!episodes.empty()) {
        if (!output.empty()) {
            output.push_back('\n');
        }
        output.append("Past Experiences:\n");
        for (const RetrievedEpisodeCandidate& episode : episodes) {
            output.append("- ");
            output.append(episode.summary_compressed);
            output.push_back('\n');
        }
    }
    return output;
}

std::string
RenderRetrievedRelationshipCandidateText(const RetrievedRelationshipCandidate& relationship) {
    std::string output;
    output.append(relationship.from_text);
    output.push_back(' ');
    output.append(relationship.predicate);
    output.push_back(' ');
    output.append(relationship.to_text);
    if (relationship.weight > 0.0) {
        output.append(" (weight: ");
        output.append(std::to_string(static_cast<int>(std::round(relationship.weight))));
        output.push_back(')');
    }
    return output;
}

std::string RenderRetrievedEpisodeCandidateText(const RetrievedEpisodeCandidate& episode) {
    return episode.summary_compressed;
}

} // namespace

absl::Status MemoryOrchestrator::BeginSession(Timestamp create_time) {
    if (absl::Status status = HydratePersistentMemoryCache(); !status.ok()) {
        return status;
    }
    return PersistSessionIfNeeded(create_time);
}

absl::Status MemoryOrchestrator::HydratePersistentMemoryCache() {
    if (!store_) {
        return absl::OkStatus();
    }

    const std::string& user_id = memory_.snapshot().conversation.user_id;
    const absl::StatusOr<std::vector<Entity>> entities = store_->ListEntitiesByUser(user_id);
    if (!entities.ok()) {
        if (absl::IsUnimplemented(entities.status())) {
            return absl::OkStatus();
        }
        LOG(WARNING) << "MemoryOrchestrator failed to load entities for cache hydration"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " user_id=" << SanitizeForLog(user_id) << " detail='"
                     << SanitizeForLog(entities.status().message()) << "'";
        return entities.status();
    }

    for (const Entity& entity : *entities) {
        if (entity.active_model_text.has_value() && !entity.active_model_text->empty()) {
            memory_.UpsertActiveModel(entity.entity_id, *entity.active_model_text);
        } else if (entity.familiar_label_text.has_value() && !entity.familiar_label_text->empty()) {
            memory_.UpsertFamiliarLabel(entity.entity_id, *entity.familiar_label_text);
        }
    }

    LOG(INFO) << "MemoryOrchestrator hydrated persistent memory cache"
              << " session_id=" << SanitizeForLog(session_id_)
              << " entity_count=" << entities->size();
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::ValidateTurnText(std::string_view session_id,
                                                  std::string_view turn_id,
                                                  std::string_view role_label) const {
    if (session_id.empty()) {
        return invalid_argument(std::string("gateway ") + std::string(role_label) +
                                " text must include a session_id");
    }
    if (turn_id.empty()) {
        return invalid_argument(std::string("gateway ") + std::string(role_label) +
                                " text must include a turn_id");
    }
    if (session_id != session_id_) {
        LOG(WARNING) << "MemoryOrchestrator rejected turn text for mismatched session"
                     << " expected_session_id=" << SanitizeForLog(session_id_)
                     << " received_session_id=" << SanitizeForLog(session_id);
        return invalid_argument("gateway turn text session_id does not match orchestrator session");
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::ValidateSessionReadyForPersistence() const {
    if (store_ != nullptr && !session_persisted_) {
        return absl::FailedPreconditionError(
            "memory orchestrator requires BeginSession before handling conversation messages");
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistSessionIfNeeded(Timestamp create_time) {
    if (!store_ || session_persisted_) {
        return absl::OkStatus();
    }

    const WorkingMemoryState& state = memory_.snapshot();
    const MemorySessionRecord session_record{
        .session_id = session_id_,
        .user_id = state.conversation.user_id,
        .system_prompt = state.system_prompt.base_instructions,
        .created_at = create_time,
        .ended_at = std::nullopt,
    };
    if (absl::Status status = ValidateMemorySessionRecord(session_record); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->UpsertSession(session_record); !status.ok()) {
        LOG(WARNING)
            << "MemoryOrchestrator store.UpsertSession failed while persisting the session record"
            << " session_id=" << SanitizeForLog(session_id_)
            << " user_id=" << SanitizeForLog(state.conversation.user_id)
            << " session_created_at=" << SanitizeForLog(FormatTimestamp(create_time)) << " detail='"
            << SanitizeForLog(status.message()) << "'";
        return status;
    }
    if (absl::Status status = PersistUserWorkingMemorySnapshot(create_time); !status.ok()) {
        return status;
    }
    session_persisted_ = true;
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistUserWorkingMemorySnapshot(Timestamp updated_at) {
    return PersistUserWorkingMemorySnapshot(memory_.snapshot(), updated_at);
}

absl::Status MemoryOrchestrator::PersistUserWorkingMemorySnapshot(const WorkingMemoryState& state,
                                                                  Timestamp updated_at) {
    if (!store_) {
        return absl::OkStatus();
    }

    absl::StatusOr<std::string> rendered_working_memory = RenderWorkingMemoryPrompt(state);
    if (!rendered_working_memory.ok()) {
        return rendered_working_memory.status();
    }

    const UserWorkingMemoryRecord record{
        .user_id = state.conversation.user_id,
        .session_id = session_id_,
        .working_memory = state,
        .rendered_working_memory = std::move(*rendered_working_memory),
        .updated_at = updated_at,
    };
    if (absl::Status status = ValidateUserWorkingMemoryRecord(record); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->UpsertUserWorkingMemory(record); !status.ok()) {
        LOG(WARNING) << "MemoryOrchestrator store.UpsertUserWorkingMemory failed while "
                        "persisting the current user working-memory snapshot"
                     << " session_id=" << SanitizeForLog(session_id_)
                     << " user_id=" << SanitizeForLog(state.conversation.user_id)
                     << " updated_at=" << SanitizeForLog(FormatTimestamp(updated_at)) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return status;
    }
    return absl::OkStatus();
}

absl::Status MemoryOrchestrator::PersistConversationMessage(std::string_view turn_id,
                                                            const Message& message) {
    if (!store_) {
        return absl::OkStatus();
    }

    const Conversation& conversation = memory_.conversation();
    if (conversation.items.empty()) {
        return invalid_argument(
            "cannot persist a conversation message without a conversation item");
    }
    const ConversationItem& current_item = conversation.items.back();
    if (current_item.type != ConversationItemType::OngoingEpisode ||
        !current_item.ongoing_episode.has_value() ||
        current_item.ongoing_episode->messages.empty()) {
        return invalid_argument("cannot persist a conversation message without an ongoing episode");
    }

    const std::int64_t conversation_item_index =
        static_cast<std::int64_t>(conversation.items.size() - 1U);
    const std::int64_t message_index =
        static_cast<std::int64_t>(current_item.ongoing_episode->messages.size() - 1U);
    const ConversationMessageWrite write{
        .session_id = session_id_,
        .conversation_item_index = conversation_item_index,
        .message_index = message_index,
        .turn_id = std::string(turn_id),
        .role = message.role,
        .content = message.content,
        .create_time = message.create_time,
    };
    if (absl::Status status = ValidateConversationMessageWrite(write); !status.ok()) {
        return status;
    }
    if (absl::Status status = store_->AppendConversationMessage(write); !status.ok()) {
        LOG(WARNING)
            << "MemoryOrchestrator store.AppendConversationMessage failed while appending the raw "
               "transcript message for the current ongoing episode"
            << " session_id=" << SanitizeForLog(session_id_)
            << " turn_id=" << SanitizeForLog(turn_id)
            << " role=" << (message.role == MessageRole::User ? "user" : "assistant")
            << " conversation_item_index=" << conversation_item_index
            << " message_index=" << message_index
            << " message_created_at=" << SanitizeForLog(FormatTimestamp(message.create_time))
            << " detail='" << SanitizeForLog(status.message()) << "'";
        return status;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<RetrievedMemory>> MemoryOrchestrator::RetrieveRelevantMemories(
    const Message& user_message,
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context) {
    if (!store_) {
        return std::nullopt;
    }

    const std::string& user_id = memory_.snapshot().conversation.user_id;

    std::unordered_set<std::string> seen_relationship_ids;
    std::vector<RetrievedRelationshipCandidate> relationship_candidates;
    std::unordered_set<std::string> seen_episode_ids;
    std::vector<RetrievedEpisodeCandidate> episode_candidates;
    std::unordered_set<std::string> related_entity_ids;
    std::optional<Embedding> query_embedding;
    std::size_t episodic_candidates_retrieved = 0;
    std::size_t hop_1_entities_visited = 0;
    std::size_t hop_2_entities_visited = 0;
    std::size_t hop_1_edges_selected = 0;
    std::size_t hop_2_edges_selected = 0;
    std::size_t malformed_relationship_embedding_count = 0;
    bool used_single_hop_fallback = false;

    // --- Path 1: Direct episode similarity search (embedding-based) ---
    if (retrieval_embedding_client_ != nullptr && !retrieval_embedding_model_.empty()) {
        const auto embed_query_started_at =
            isla::server::ai_gateway::TurnTelemetryContext::Clock::now();
        const absl::StatusOr<Embedding> embedded_query =
            retrieval_embedding_client_->Embed(isla::server::EmbeddingRequest{
                .model = retrieval_embedding_model_,
                .text = user_message.content,
                .output_dimensionality = kEmbeddingDimensions,
                .telemetry_context = telemetry_context,
            });
        isla::server::ai_gateway::RecordTelemetryPhase(
            telemetry_context, isla::server::ai_gateway::telemetry::kPhaseMemoryRetrievalEmbedQuery,
            embed_query_started_at, isla::server::ai_gateway::TurnTelemetryContext::Clock::now());
        if (!embedded_query.ok()) {
            LOG(WARNING) << "MemoryOrchestrator failed to embed user query for episode similarity "
                            "search"
                         << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(embedded_query.status().message()) << "'";
        } else if (!embedded_query->empty()) {
            query_embedding = *embedded_query;
            const absl::StatusOr<std::vector<LongTermEpisode>> similar_episodes =
                store_->SearchLongTermEpisodesByEmbedding(user_id, *query_embedding,
                                                          episode_similarity_search_top_k_);
            if (!similar_episodes.ok()) {
                if (!absl::IsUnimplemented(similar_episodes.status())) {
                    LOG(WARNING) << "MemoryOrchestrator failed to search long-term episodes by "
                                    "embedding"
                                 << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                                 << SanitizeForLog(similar_episodes.status().message()) << "'";
                }
            } else {
                for (const LongTermEpisode& episode : *similar_episodes) {
                    if (episode.user_id != user_id ||
                        !seen_episode_ids.insert(episode.lte_id).second) {
                        continue;
                    }
                    episode_candidates.push_back(RetrievedEpisodeCandidate{
                        .lte_id = episode.lte_id,
                        .summary_compressed = episode.summary_compressed,
                        .created_at = episode.created_at,
                    });
                    ++episodic_candidates_retrieved;
                }
            }
        }
    }

    // --- Path 2: Entity-triggered retrieval (KG facts) ---
    const PersistentMemoryCache& cache = memory_.snapshot().system_prompt.persistent_memory_cache;

    std::vector<std::string> seed_entity_ids;
    if (!cache.active_models.empty() || !cache.familiar_labels.empty()) {
        std::unordered_set<std::string> seen_entity_ids;
        std::vector<std::string> entity_ids;
        entity_ids.reserve(cache.active_models.size() + cache.familiar_labels.size());
        for (const ActiveModel& model : cache.active_models) {
            if (seen_entity_ids.insert(model.entity_id).second) {
                entity_ids.push_back(model.entity_id);
            }
        }
        for (const FamiliarLabel& label : cache.familiar_labels) {
            if (seen_entity_ids.insert(label.entity_id).second) {
                entity_ids.push_back(label.entity_id);
            }
        }

        seed_entity_ids = entity_ids;
        absl::StatusOr<std::unordered_map<std::string, std::optional<Entity>>> cache_entities =
            LoadEntityMapForRetrievedMemory(store_.get(), entity_ids);
        if (cache_entities.ok()) {
            seed_entity_ids =
                SelectSeedEntityIdsForRetrievedMemory(user_message, entity_ids, *cache_entities);
        } else {
            LOG(WARNING) << "MemoryOrchestrator failed to load cache entity labels for query-aware "
                            "retrieval; falling back to cached entity ids"
                         << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(cache_entities.status().message()) << "'";
        }
    }
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalSeedEntityCount,
        static_cast<double>(seed_entity_ids.size()));

    const auto append_relationship_candidate = [&](const Relationship& relationship) {
        related_entity_ids.insert(relationship.from_entity_id);
        related_entity_ids.insert(relationship.to_entity_id);
        relationship_candidates.push_back(RetrievedRelationshipCandidate{
            .relationship_id = relationship.relationship_id,
            .from_text = relationship.from_entity_id,
            .predicate = relationship.predicate,
            .to_text = relationship.to_entity_id,
            .weight = relationship.weight,
        });
    };

    if (query_embedding.has_value()) {
        std::unordered_set<std::string> seed_entity_id_set(seed_entity_ids.begin(),
                                                           seed_entity_ids.end());
        std::unordered_set<std::string> seen_hop_1_entity_ids;
        std::vector<std::string> hop_1_entity_ids;
        const auto collect_rankable_relationships =
            [&](const std::vector<Relationship>& relationships) {
                std::vector<Relationship> eligible_relationships;
                eligible_relationships.reserve(relationships.size());
                for (const Relationship& rel : relationships) {
                    if (rel.is_archived || rel.user_id != user_id ||
                        seen_relationship_ids.contains(rel.relationship_id)) {
                        continue;
                    }
                    eligible_relationships.push_back(rel);
                }
                return eligible_relationships;
            };

        for (const std::string& entity_id : seed_entity_ids) {
            ++hop_1_entities_visited;
            const absl::StatusOr<std::vector<Relationship>> relationships =
                store_->ListRelationshipsForEntity(entity_id);
            if (!relationships.ok()) {
                if (!absl::IsUnimplemented(relationships.status())) {
                    LOG(WARNING) << "MemoryOrchestrator failed to retrieve relationships for entity"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " entity_id=" << SanitizeForLog(entity_id) << " detail='"
                                 << SanitizeForLog(relationships.status().message()) << "'";
                }
                continue;
            }

            const std::vector<Relationship> eligible_relationships =
                collect_rankable_relationships(*relationships);
            RetrievalRankingStats ranking_stats;
            for (const Relationship& rel :
                 RankEdgesBySimilarity(*query_embedding, eligible_relationships,
                                       kg_hop_1_top_k_per_entity_, &ranking_stats)) {
                if (!seen_relationship_ids.insert(rel.relationship_id).second) {
                    continue;
                }
                append_relationship_candidate(rel);
                ++hop_1_edges_selected;

                const auto maybe_enqueue_hop_1_entity =
                    [&](const std::string& candidate_entity_id) {
                        if (seed_entity_id_set.contains(candidate_entity_id) ||
                            !seen_hop_1_entity_ids.insert(candidate_entity_id).second) {
                            return;
                        }
                        hop_1_entity_ids.push_back(candidate_entity_id);
                    };
                maybe_enqueue_hop_1_entity(rel.from_entity_id);
                maybe_enqueue_hop_1_entity(rel.to_entity_id);
            }
            malformed_relationship_embedding_count += ranking_stats.malformed_embedding_count;
        }

        for (const std::string& entity_id : hop_1_entity_ids) {
            ++hop_2_entities_visited;
            const absl::StatusOr<std::vector<Relationship>> relationships =
                store_->ListRelationshipsForEntity(entity_id);
            if (!relationships.ok()) {
                if (!absl::IsUnimplemented(relationships.status())) {
                    LOG(WARNING) << "MemoryOrchestrator failed to retrieve relationships for hop-1 "
                                    "entity"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " entity_id=" << SanitizeForLog(entity_id) << " detail='"
                                 << SanitizeForLog(relationships.status().message()) << "'";
                }
                continue;
            }

            const std::vector<Relationship> eligible_relationships =
                collect_rankable_relationships(*relationships);
            RetrievalRankingStats ranking_stats;
            for (const Relationship& rel :
                 RankEdgesBySimilarity(*query_embedding, eligible_relationships,
                                       kg_hop_2_top_k_per_entity_, &ranking_stats)) {
                if (!seen_relationship_ids.insert(rel.relationship_id).second) {
                    continue;
                }
                append_relationship_candidate(rel);
                ++hop_2_edges_selected;
            }
            malformed_relationship_embedding_count += ranking_stats.malformed_embedding_count;
        }
    } else {
        used_single_hop_fallback = true;
        for (const std::string& entity_id : seed_entity_ids) {
            const absl::StatusOr<std::vector<Relationship>> relationships =
                store_->ListRelationshipsForEntity(entity_id);
            if (!relationships.ok()) {
                if (!absl::IsUnimplemented(relationships.status())) {
                    LOG(WARNING) << "MemoryOrchestrator failed to retrieve relationships for entity"
                                 << " session_id=" << SanitizeForLog(session_id_)
                                 << " entity_id=" << SanitizeForLog(entity_id) << " detail='"
                                 << SanitizeForLog(relationships.status().message()) << "'";
                }
                continue;
            }

            for (const Relationship& rel : *relationships) {
                if (rel.is_archived || rel.user_id != user_id ||
                    !seen_relationship_ids.insert(rel.relationship_id).second) {
                    continue;
                }
                append_relationship_candidate(rel);
            }
        }

        std::sort(relationship_candidates.begin(), relationship_candidates.end(),
                  [](const RetrievedRelationshipCandidate& lhs,
                     const RetrievedRelationshipCandidate& rhs) {
                      if (lhs.weight != rhs.weight) {
                          return lhs.weight > rhs.weight;
                      }
                      return lhs.relationship_id < rhs.relationship_id;
                  });
    }

    const std::vector<std::string> related_entity_ids_vector(related_entity_ids.begin(),
                                                             related_entity_ids.end());
    absl::StatusOr<std::unordered_map<std::string, std::optional<Entity>>> related_entities =
        LoadEntityMapForRetrievedMemory(store_.get(), related_entity_ids_vector);
    if (related_entities.ok()) {
        for (RetrievedRelationshipCandidate& relationship : relationship_candidates) {
            if (const auto from_it = related_entities->find(relationship.from_text);
                from_it != related_entities->end() && from_it->second.has_value()) {
                relationship.from_text = from_it->second->label;
            }
            if (const auto to_it = related_entities->find(relationship.to_text);
                to_it != related_entities->end() && to_it->second.has_value()) {
                relationship.to_text = to_it->second->label;
            }
        }
    } else {
        LOG(WARNING) << "MemoryOrchestrator failed to retrieve entity labels for long-term context"
                     << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                     << SanitizeForLog(related_entities.status().message()) << "'";
    }
    // Note: episode_candidates are intentionally NOT re-sorted here. They arrive from
    // similarity search in ascending cosine distance order (most relevant first), and we
    // want to preserve that ranking in the fallback path when the reranker is unavailable.
    const std::size_t total_candidates_before_rerank =
        relationship_candidates.size() + episode_candidates.size();
    std::size_t total_candidates_after_rerank = total_candidates_before_rerank;

    if (retrieved_memory_reranker_ != nullptr &&
        (!relationship_candidates.empty() || !episode_candidates.empty())) {
        std::vector<RankedRetrievedMemoryCandidate> ranked_candidates;
        ranked_candidates.reserve(relationship_candidates.size() + episode_candidates.size());
        std::vector<RetrievedMemoryCandidate> reranker_candidates;
        reranker_candidates.reserve(relationship_candidates.size() + episode_candidates.size());

        for (const RetrievedRelationshipCandidate& relationship : relationship_candidates) {
            RetrievedMemoryCandidate candidate{
                .kind = RetrievedMemoryCandidateKind::KgFact,
                .source_id = relationship.relationship_id,
                .candidate_text = RenderRetrievedRelationshipCandidateText(relationship),
            };
            reranker_candidates.push_back(candidate);
            ranked_candidates.push_back(RankedRetrievedMemoryCandidate{
                .candidate = std::move(candidate),
                .relationship = relationship,
            });
        }
        for (const RetrievedEpisodeCandidate& episode : episode_candidates) {
            RetrievedMemoryCandidate candidate{
                .kind = RetrievedMemoryCandidateKind::Episode,
                .source_id = episode.lte_id,
                .candidate_text = RenderRetrievedEpisodeCandidateText(episode),
            };
            reranker_candidates.push_back(candidate);
            ranked_candidates.push_back(RankedRetrievedMemoryCandidate{
                .candidate = std::move(candidate),
                .episode = episode,
            });
        }

        const auto rerank_started_at = isla::server::ai_gateway::TurnTelemetryContext::Clock::now();
        absl::StatusOr<std::vector<double>> scores =
            retrieved_memory_reranker_->Score(user_message.content, reranker_candidates);
        isla::server::ai_gateway::RecordTelemetryPhase(
            telemetry_context, isla::server::ai_gateway::telemetry::kPhaseMemoryRetrievalRerank,
            rerank_started_at, isla::server::ai_gateway::TurnTelemetryContext::Clock::now());
        if (!scores.ok()) {
            LOG(WARNING) << "MemoryOrchestrator failed to rerank long-term retrieval candidates"
                         << " session_id=" << SanitizeForLog(session_id_) << " detail='"
                         << SanitizeForLog(scores.status().message())
                         << "'; falling back to unreranked retrieval output";
        } else if (scores->size() != ranked_candidates.size()) {
            LOG(WARNING) << "MemoryOrchestrator received mismatched reranker scores for long-term "
                            "retrieval candidates"
                         << " session_id=" << SanitizeForLog(session_id_)
                         << " candidate_count=" << ranked_candidates.size()
                         << " score_count=" << scores->size()
                         << "; falling back to unreranked retrieval output";
        } else {
            for (std::size_t i = 0; i < ranked_candidates.size(); ++i) {
                ranked_candidates[i].score = SanitizeRetrievedMemoryRerankerScore((*scores)[i]);
            }
            std::sort(ranked_candidates.begin(), ranked_candidates.end(),
                      [](const RankedRetrievedMemoryCandidate& lhs,
                         const RankedRetrievedMemoryCandidate& rhs) {
                          if (lhs.score != rhs.score) {
                              return lhs.score > rhs.score;
                          }
                          if (lhs.candidate.kind != rhs.candidate.kind) {
                              return lhs.candidate.kind < rhs.candidate.kind;
                          }
                          if (lhs.candidate.source_id != rhs.candidate.source_id) {
                              return lhs.candidate.source_id < rhs.candidate.source_id;
                          }
                          return lhs.candidate.candidate_text < rhs.candidate.candidate_text;
                      });

            relationship_candidates.clear();
            episode_candidates.clear();
            for (RankedRetrievedMemoryCandidate& ranked_candidate : ranked_candidates) {
                if (ranked_candidate.score < retrieved_memory_reranker_min_score_) {
                    continue;
                }
                if (ranked_candidate.relationship.has_value()) {
                    relationship_candidates.push_back(*ranked_candidate.relationship);
                } else if (ranked_candidate.episode.has_value()) {
                    episode_candidates.push_back(std::move(*ranked_candidate.episode));
                }
            }
        }
    }
    total_candidates_after_rerank = relationship_candidates.size() + episode_candidates.size();
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalKgHop1EntitiesVisited,
        static_cast<double>(hop_1_entities_visited));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalKgHop2EntitiesVisited,
        static_cast<double>(hop_2_entities_visited));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalKgHop1EdgesSelected,
        static_cast<double>(hop_1_edges_selected));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalKgHop2EdgesSelected,
        static_cast<double>(hop_2_edges_selected));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalEpisodicCandidatesRetrieved,
        static_cast<double>(episodic_candidates_retrieved));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalTotalCandidatesBeforeRerank,
        static_cast<double>(total_candidates_before_rerank));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalTotalCandidatesAfterRerank,
        static_cast<double>(total_candidates_after_rerank));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalInjectedKgCount,
        static_cast<double>(relationship_candidates.size()));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalInjectedEpisodeCount,
        static_cast<double>(episode_candidates.size()));
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::kMeasurementMemoryRetrievalKgFallbackSingleHopUsed,
        used_single_hop_fallback ? 1.0 : 0.0);
    isla::server::ai_gateway::RecordTelemetryMeasurement(
        telemetry_context,
        isla::server::ai_gateway::telemetry::
            kMeasurementMemoryRetrievalRelationshipEmbeddingMalformedCount,
        static_cast<double>(malformed_relationship_embedding_count));

    std::string rendered_context =
        RenderRetrievedMemory(relationship_candidates, episode_candidates);
    if (rendered_context.empty()) {
        return std::nullopt;
    }
    return rendered_context;
}

} // namespace isla::server::memory
