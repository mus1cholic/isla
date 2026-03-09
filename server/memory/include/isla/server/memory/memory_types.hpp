#pragma once

#include <optional>
#include <string>
#include <vector>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::memory {

using Embedding = std::vector<double>;
using RetrievedMemory = std::string;

enum class MessageRole {
    User,
    Assistant,
    Stub,
};

enum class LongTermEpisodeOutcome {
    Resolved,
    Abandoned,
    Ongoing,
    Informational,
};

struct ActiveModel {
    std::string entity_id;
    std::string text;
};

struct FamiliarLabel {
    std::string entity_id;
    std::string text;
};

struct PersistentMemoryCache {
    std::vector<ActiveModel> active_models;
    std::vector<FamiliarLabel> familiar_labels;
};

struct Message {
    MessageRole role = MessageRole::User;
    std::string content;
    Timestamp create_time;
};

struct Conversation {
    std::vector<Message> messages;
    std::string user_id;
    std::string conversation_id;
    std::string session_id;
};

struct Episode {
    std::string episode_id;
    std::optional<std::string> tier1_detail;
    std::string tier2_summary;
    std::string tier3_ref;
    std::vector<std::string> tier3_keywords;
    int salience = 0;
    Embedding embedding;
    Timestamp created_at;
};

struct Entity {
    std::string entity_id;
    std::string label;
    std::string category;
    int activeness = 0;
    Timestamp created_at;
    Timestamp updated_at;
};

struct Relationship {
    std::string relationship_id;
    std::string from;
    std::string predicate;
    std::string to;
    double weight = 0.0;
    int observation_count = 0;
    Timestamp last_observed_at;
    std::vector<std::string> source_episode_ids;
    Embedding embedding;
};

struct LongTermEpisode {
    std::string lte_id;
    std::string summary_full;
    std::string summary_compressed;
    std::vector<std::string> keywords;
    Embedding embedding;
    std::vector<std::string> related_entities;
    LongTermEpisodeOutcome outcome = LongTermEpisodeOutcome::Informational;
    int complexity = 0;
    Timestamp created_at;
    std::vector<std::string> original_episode_ids;
    std::optional<std::string> caused_by;
    std::optional<std::string> led_to;
};

struct Session {
    std::string session_id;
    std::string system_prompt;
    PersistentMemoryCache persistent_memory_cache;
    std::vector<Episode> mid_term_episodes;
    // TODO: Replace this placeholder string with structured retrieved-memory
    // types once the retrieval payload schema is defined.
    std::optional<RetrievedMemory> retrieved_memory;
    Conversation conversation;
    Timestamp created_at;
    std::optional<Timestamp> ended_at;
};

NLOHMANN_JSON_SERIALIZE_ENUM(MessageRole, {
                                              { MessageRole::User, "user" },
                                              { MessageRole::Assistant, "assistant" },
                                              { MessageRole::Stub, "stub" },
                                          })

NLOHMANN_JSON_SERIALIZE_ENUM(LongTermEpisodeOutcome,
                             {
                                 { LongTermEpisodeOutcome::Resolved, "resolved" },
                                 { LongTermEpisodeOutcome::Abandoned, "abandoned" },
                                 { LongTermEpisodeOutcome::Ongoing, "ongoing" },
                                 { LongTermEpisodeOutcome::Informational, "informational" },
                             })

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ActiveModel, entity_id, text)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FamiliarLabel, entity_id, text)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PersistentMemoryCache, active_models,
                                                familiar_labels)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Message, role, content, create_time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Conversation, messages, user_id, conversation_id,
                                                session_id)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Episode, episode_id, tier1_detail, tier2_summary,
                                                tier3_ref, tier3_keywords, salience, embedding,
                                                created_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Entity, entity_id, label, category, activeness,
                                                created_at, updated_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Relationship, relationship_id, from, predicate, to,
                                                weight, observation_count, last_observed_at,
                                                source_episode_ids, embedding)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LongTermEpisode, lte_id, summary_full,
                                                summary_compressed, keywords, embedding,
                                                related_entities, outcome, complexity, created_at,
                                                original_episode_ids, caused_by, led_to)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Session, session_id, system_prompt,
                                                persistent_memory_cache, mid_term_episodes,
                                                retrieved_memory, conversation, created_at,
                                                ended_at)

} // namespace isla::server::memory
