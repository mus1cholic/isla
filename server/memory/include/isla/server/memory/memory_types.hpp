#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::memory {

using Embedding = std::vector<double>;
using RetrievedMemory = std::string;
inline constexpr int kExpandableEpisodeSalienceThreshold = 8;

enum class MessageRole {
    User,
    Assistant,
};

enum class ConversationItemType {
    OngoingEpisode,
    EpisodeStub,
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

struct SystemPromptState {
    std::string base_instructions;
    PersistentMemoryCache persistent_memory_cache;
};

struct Message {
    MessageRole role = MessageRole::User;
    std::string content;
    Timestamp create_time;
};

struct OngoingEpisode {
    std::vector<Message> messages;
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

struct EpisodeStub {
    std::string content;
    Timestamp create_time;
};

struct ConversationItem {
    ConversationItemType type = ConversationItemType::OngoingEpisode;
    std::optional<OngoingEpisode> ongoing_episode;
    std::optional<EpisodeStub> episode_stub;
};

struct Conversation {
    std::vector<ConversationItem> items;
    std::string user_id;
};

struct WorkingMemoryState {
    // Stable high-priority prompt context: bundled/configured base instructions plus
    // persistent-memory cache entries that should ride alongside those instructions.
    SystemPromptState system_prompt;
    std::vector<Episode> mid_term_episodes;
    // TODO: Replace this placeholder string with structured retrieved-memory
    // types once the retrieval payload schema is defined.
    std::optional<RetrievedMemory> retrieved_memory;
    Conversation conversation;
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
    WorkingMemoryState working_memory;
    Timestamp created_at;
    std::optional<Timestamp> ended_at;
};

[[nodiscard]] inline bool IsExpandableEpisode(const Episode& episode) {
    return episode.salience >= kExpandableEpisodeSalienceThreshold &&
           episode.tier1_detail.has_value() && !episode.tier1_detail->empty();
}

NLOHMANN_JSON_SERIALIZE_ENUM(MessageRole, {
                                              { MessageRole::User, "user" },
                                              { MessageRole::Assistant, "assistant" },
                                          })

NLOHMANN_JSON_SERIALIZE_ENUM(ConversationItemType,
                             {
                                 { ConversationItemType::OngoingEpisode, "ongoing_episode" },
                                 { ConversationItemType::EpisodeStub, "episode_stub" },
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SystemPromptState, base_instructions,
                                                persistent_memory_cache)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Message, role, content, create_time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OngoingEpisode, messages)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkingMemoryState, system_prompt,
                                                mid_term_episodes, retrieved_memory, conversation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Episode, episode_id, tier1_detail, tier2_summary,
                                                tier3_ref, tier3_keywords, salience, embedding,
                                                created_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EpisodeStub, content, create_time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Entity, entity_id, label, category, activeness,
                                                created_at, updated_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Relationship, relationship_id, from, predicate, to,
                                                weight, observation_count, last_observed_at,
                                                source_episode_ids, embedding)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LongTermEpisode, lte_id, summary_full,
                                                summary_compressed, keywords, embedding,
                                                related_entities, outcome, complexity, created_at,
                                                original_episode_ids, caused_by, led_to)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Session, session_id, working_memory, created_at,
                                                ended_at)

inline void to_json(nlohmann::json& j, const ConversationItem& value) {
    j = nlohmann::json{ { "type", value.type } };
    switch (value.type) {
    case ConversationItemType::OngoingEpisode:
        if (!value.ongoing_episode.has_value()) {
            throw std::invalid_argument(
                "ConversationItem type=ongoing_episode is missing ongoing_episode payload");
        }
        j["ongoing_episode"] = *value.ongoing_episode;
        break;
    case ConversationItemType::EpisodeStub:
        if (!value.episode_stub.has_value()) {
            throw std::invalid_argument(
                "ConversationItem type=episode_stub is missing episode_stub payload");
        }
        j["episode_stub"] = *value.episode_stub;
        break;
    }
}

inline void from_json(const nlohmann::json& j, ConversationItem& value) {
    j.at("type").get_to(value.type);
    value.ongoing_episode.reset();
    value.episode_stub.reset();
    switch (value.type) {
    case ConversationItemType::OngoingEpisode:
        value.ongoing_episode = j.at("ongoing_episode").get<OngoingEpisode>();
        break;
    case ConversationItemType::EpisodeStub:
        value.episode_stub = j.at("episode_stub").get<EpisodeStub>();
        break;
    }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Conversation, items, user_id)

} // namespace isla::server::memory
