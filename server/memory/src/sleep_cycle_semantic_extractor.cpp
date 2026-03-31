#include "isla/server/memory/sleep_cycle_semantic_extractor.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/llm_json_utils.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "nlohmann/json.hpp"

namespace isla::server::memory {
namespace {

using isla::server::LlmClient;
using isla::server::LlmEvent;
using isla::server::LlmRequest;
using isla::server::LlmTextDeltaEvent;
using isla::server::ai_gateway::SanitizeForLog;
using nlohmann::json;

constexpr std::size_t kMaxSemanticEntitiesPerExtraction = 128;
constexpr std::size_t kMaxSemanticRelationshipsPerExtraction = 256;
constexpr std::size_t kMaxSourceEpisodeIdsPerSemanticItem = 16;
constexpr std::size_t kMaxExistingRelationshipsPerExtractionRequest = 256;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

template <std::size_t N>
absl::Status ValidateExactObjectKeys(const json& object,
                                     const std::array<std::string_view, N>& keys,
                                     std::string_view context) {
    if (!object.is_object()) {
        return invalid_argument(std::string(context) + " must be a JSON object");
    }
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        bool found = false;
        for (std::string_view expected_key : keys) {
            if (key == expected_key) {
                found = true;
                break;
            }
        }
        if (!found) {
            return invalid_argument(std::string(context) + " contains unexpected field '" + key +
                                    "'");
        }
    }
    for (std::string_view expected_key : keys) {
        if (!object.contains(expected_key)) {
            return invalid_argument(std::string(context) + " is missing required field '" +
                                    std::string(expected_key) + "'");
        }
    }
    return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> ParseRequiredStringArray(const json& object,
                                                                  std::string_view field_name,
                                                                  std::string_view context) {
    if (!object.at(field_name).is_array()) {
        return invalid_argument(std::string(context) + " field '" + std::string(field_name) +
                                "' must be an array");
    }
    if (object.at(field_name).size() > kMaxSourceEpisodeIdsPerSemanticItem) {
        return invalid_argument(std::string(context) + " field '" + std::string(field_name) +
                                "' exceeds max size");
    }
    std::vector<std::string> values;
    values.reserve(object.at(field_name).size());
    for (const json& value : object.at(field_name)) {
        if (!value.is_string()) {
            return invalid_argument(std::string(context) + " field '" + std::string(field_name) +
                                    "' must contain only strings");
        }
        const std::string parsed = value.get<std::string>();
        if (parsed.empty()) {
            return invalid_argument(std::string(context) + " field '" + std::string(field_name) +
                                    "' must not contain empty strings");
        }
        values.push_back(parsed);
    }
    return values;
}

json SerializeRequest(const SleepCycleSemanticExtractionRequest& request) {
    json episodes = json::array();
    for (const Episode& episode : request.mid_term_episodes) {
        episodes.push_back(json{
            { "episode_id", episode.episode_id },
            { "tier1_detail", episode.tier1_detail },
            { "tier2_summary", episode.tier2_summary },
            { "tier3_ref", episode.tier3_ref },
            { "tier3_keywords", episode.tier3_keywords },
            { "created_at", episode.created_at },
        });
    }
    json existing_relationships = json::array();
    for (const ExistingSemanticRelationship& relationship : request.existing_relationships) {
        existing_relationships.push_back(json{
            { "relationship_id", relationship.relationship_id },
            { "from_label", relationship.from_label },
            { "from_category", relationship.from_category },
            { "predicate", relationship.predicate },
            { "to_label", relationship.to_label },
            { "to_category", relationship.to_category },
            { "weight", relationship.weight },
            { "last_observed_at", relationship.last_observed_at },
        });
    }
    return json{
        { "user_id", request.user_id },
        { "episodes", std::move(episodes) },
        { "existing_relationships", std::move(existing_relationships) },
    };
}

absl::Status ValidateSourceEpisodeIds(const std::vector<std::string>& source_episode_ids,
                                      const std::unordered_set<std::string>& valid_episode_ids,
                                      std::string_view context) {
    if (source_episode_ids.empty()) {
        return invalid_argument(std::string(context) +
                                " must reference at least one source_episode_id");
    }
    for (const std::string& source_episode_id : source_episode_ids) {
        if (!valid_episode_ids.contains(source_episode_id)) {
            return invalid_argument(std::string(context) +
                                    " references unknown source_episode_id '" + source_episode_id +
                                    "'");
        }
    }
    return absl::OkStatus();
}

absl::StatusOr<SleepCycleSemanticExtractionResult>
ParseResponse(const std::string& response_text,
              const std::unordered_set<std::string>& valid_episode_ids,
              const std::unordered_set<std::string>& valid_relationship_ids) {
    json response;
    try {
        response = json::parse(response_text);
    } catch (const json::parse_error& error) {
        const std::string stripped = StripMarkdownCodeFences(response_text);
        if (stripped == response_text) {
            return invalid_argument(std::string("sleep-cycle semantic extractor returned invalid "
                                                "JSON: ") +
                                    error.what());
        }
        try {
            response = json::parse(stripped);
        } catch (const json::parse_error& inner_error) {
            return invalid_argument(
                std::string("sleep-cycle semantic extractor returned invalid JSON (after stripping "
                            "code fences): ") +
                inner_error.what());
        }
    }

    constexpr std::array<std::string_view, 2> kTopLevelKeys = { "entities", "relationships" };
    if (absl::Status status =
            ValidateExactObjectKeys(response, kTopLevelKeys, "sleep-cycle semantic extractor");
        !status.ok()) {
        return status;
    }
    if (!response.at("entities").is_array()) {
        return invalid_argument("sleep-cycle semantic extractor field 'entities' must be an array");
    }
    if (!response.at("relationships").is_array()) {
        return invalid_argument(
            "sleep-cycle semantic extractor field 'relationships' must be an array");
    }
    if (response.at("entities").size() > kMaxSemanticEntitiesPerExtraction) {
        return invalid_argument("sleep-cycle semantic extractor field 'entities' exceeds max size");
    }
    if (response.at("relationships").size() > kMaxSemanticRelationshipsPerExtraction) {
        return invalid_argument(
            "sleep-cycle semantic extractor field 'relationships' exceeds max size");
    }

    SleepCycleSemanticExtractionResult result;
    constexpr std::array<std::string_view, 3> kEntityKeys = { "label", "category",
                                                              "source_episode_ids" };
    for (const json& entity_json : response.at("entities")) {
        if (absl::Status status = ValidateExactObjectKeys(entity_json, kEntityKeys,
                                                          "sleep-cycle semantic extractor entity");
            !status.ok()) {
            return status;
        }
        if (!entity_json.at("label").is_string() ||
            entity_json.at("label").get<std::string>().empty()) {
            return invalid_argument(
                "sleep-cycle semantic extractor entity field 'label' must be a non-empty string");
        }
        if (!entity_json.at("category").is_string() ||
            entity_json.at("category").get<std::string>().empty()) {
            return invalid_argument(
                "sleep-cycle semantic extractor entity field 'category' must be a non-empty "
                "string");
        }
        absl::StatusOr<std::vector<std::string>> source_episode_ids = ParseRequiredStringArray(
            entity_json, "source_episode_ids", "sleep-cycle semantic extractor entity");
        if (!source_episode_ids.ok()) {
            return source_episode_ids.status();
        }
        if (absl::Status status = ValidateSourceEpisodeIds(*source_episode_ids, valid_episode_ids,
                                                           "sleep-cycle semantic extractor entity");
            !status.ok()) {
            return status;
        }
        result.entities.push_back(SemanticEntityCandidate{
            .label = entity_json.at("label").get<std::string>(),
            .category = entity_json.at("category").get<std::string>(),
            .source_episode_ids = std::move(*source_episode_ids),
        });
    }

    constexpr std::array<std::string_view, 9> kRelationshipKeys = {
        "from_label", "from_category",          "predicate", "to_label",           "to_category",
        "operation",  "target_relationship_id", "evidence",  "source_episode_ids",
    };
    constexpr std::array<std::string_view, 3> kAllowedEvidenceValues = {
        "EXPLICIT_STATEMENT",
        "STRONG_INFERENCE",
        "WEAK_INFERENCE",
    };
    constexpr std::array<std::string_view, 3> kAllowedOperationValues = {
        "APPEND",
        "STRENGTHEN",
        "SUPERSEDE",
    };
    for (const json& relationship_json : response.at("relationships")) {
        if (absl::Status status =
                ValidateExactObjectKeys(relationship_json, kRelationshipKeys,
                                        "sleep-cycle semantic extractor relationship");
            !status.ok()) {
            return status;
        }
        for (std::string_view field_name : std::array<std::string_view, 5>{
                 "from_label", "from_category", "predicate", "to_label", "to_category" }) {
            if (!relationship_json.at(field_name).is_string() ||
                relationship_json.at(field_name).get<std::string>().empty()) {
                return invalid_argument("sleep-cycle semantic extractor relationship field '" +
                                        std::string(field_name) + "' must be a non-empty string");
            }
        }
        if (!relationship_json.at("evidence").is_string()) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'evidence' must be a string");
        }
        if (!relationship_json.at("operation").is_string()) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'operation' must be a string");
        }
        const std::string operation_value = relationship_json.at("operation").get<std::string>();
        if (operation_value.empty()) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'operation' must be a "
                "non-empty string");
        }
        bool allowed_operation_value = false;
        for (std::string_view allowed_value : kAllowedOperationValues) {
            if (operation_value == allowed_value) {
                allowed_operation_value = true;
                break;
            }
        }
        if (!allowed_operation_value) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'operation' must be one of "
                "APPEND, STRENGTHEN, or SUPERSEDE");
        }
        const bool target_relationship_id_is_null =
            relationship_json.at("target_relationship_id").is_null();
        if (!target_relationship_id_is_null &&
            !relationship_json.at("target_relationship_id").is_string()) {
            return invalid_argument("sleep-cycle semantic extractor relationship field "
                                    "'target_relationship_id' must be null or a string");
        }
        const std::optional<std::string> target_relationship_id =
            target_relationship_id_is_null
                ? std::nullopt
                : std::optional<std::string>(
                      relationship_json.at("target_relationship_id").get<std::string>());
        if (operation_value == "APPEND" && target_relationship_id.has_value()) {
            return invalid_argument(
                "sleep-cycle semantic extractor APPEND relationship must use null "
                "target_relationship_id");
        }
        if ((operation_value == "STRENGTHEN" || operation_value == "SUPERSEDE") &&
            (!target_relationship_id.has_value() || target_relationship_id->empty())) {
            return invalid_argument(
                "sleep-cycle semantic extractor STRENGTHEN/SUPERSEDE relationship must include "
                "a non-empty target_relationship_id");
        }
        if (target_relationship_id.has_value() &&
            !valid_relationship_ids.contains(*target_relationship_id)) {
            return invalid_argument("sleep-cycle semantic extractor relationship references "
                                    "unknown target_relationship_id '" +
                                    *target_relationship_id + "'");
        }
        const std::string evidence_value = relationship_json.at("evidence").get<std::string>();
        if (evidence_value.empty()) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'evidence' must be a "
                "non-empty string");
        }
        bool allowed_evidence_value = false;
        for (std::string_view allowed_value : kAllowedEvidenceValues) {
            if (evidence_value == allowed_value) {
                allowed_evidence_value = true;
                break;
            }
        }
        if (!allowed_evidence_value) {
            return invalid_argument(
                "sleep-cycle semantic extractor relationship field 'evidence' must be one of "
                "EXPLICIT_STATEMENT, STRONG_INFERENCE, or WEAK_INFERENCE");
        }
        absl::StatusOr<std::vector<std::string>> source_episode_ids = ParseRequiredStringArray(
            relationship_json, "source_episode_ids", "sleep-cycle semantic extractor relationship");
        if (!source_episode_ids.ok()) {
            return source_episode_ids.status();
        }
        if (absl::Status status =
                ValidateSourceEpisodeIds(*source_episode_ids, valid_episode_ids,
                                         "sleep-cycle semantic extractor relationship");
            !status.ok()) {
            return status;
        }
        SemanticRelationshipObservation relationship;
        try {
            relationship = relationship_json.get<SemanticRelationshipObservation>();
        } catch (const std::exception& error) {
            return invalid_argument(std::string("sleep-cycle semantic extractor relationship was "
                                                "malformed: ") +
                                    error.what());
        }
        result.relationships.push_back(std::move(relationship));
    }

    return result;
}

class LlmSleepCycleSemanticExtractor final : public SleepCycleSemanticExtractor {
  public:
    LlmSleepCycleSemanticExtractor(std::shared_ptr<const LlmClient> llm_client, std::string model,
                                   std::string system_prompt,
                                   isla::server::LlmReasoningEffort reasoning_effort)
        : llm_client_(std::move(llm_client)), model_(std::move(model)),
          system_prompt_(std::move(system_prompt)), reasoning_effort_(reasoning_effort) {}

    [[nodiscard]] absl::StatusOr<SleepCycleSemanticExtractionResult>
    Extract(const SleepCycleSemanticExtractionRequest& request) override {
        if (request.user_id.empty()) {
            return invalid_argument("sleep-cycle semantic extraction requires a non-empty user_id");
        }
        std::unordered_set<std::string> valid_episode_ids;
        std::unordered_set<std::string> valid_relationship_ids;
        for (const Episode& episode : request.mid_term_episodes) {
            if (episode.episode_id.empty()) {
                return invalid_argument(
                    "sleep-cycle semantic extraction requires all episodes to have an episode_id");
            }
            valid_episode_ids.insert(episode.episode_id);
        }
        if (request.existing_relationships.size() > kMaxExistingRelationshipsPerExtractionRequest) {
            return invalid_argument(
                "sleep-cycle semantic extraction request existing_relationships exceeds max size");
        }
        for (const ExistingSemanticRelationship& relationship : request.existing_relationships) {
            if (relationship.relationship_id.empty()) {
                return invalid_argument("sleep-cycle semantic extraction requires existing "
                                        "relationships to have relationship_id");
            }
            valid_relationship_ids.insert(relationship.relationship_id);
        }

        const std::string user_text = SerializeRequest(request).dump();
        std::string output_text;
        absl::Status stream_status = llm_client_->StreamResponse(
            LlmRequest{
                .model = model_,
                .system_prompt = system_prompt_,
                .user_text = user_text,
                .reasoning_effort = reasoning_effort_,
            },
            [&output_text](const LlmEvent& event) -> absl::Status {
                return std::visit(
                    [&output_text](const auto& concrete_event) -> absl::Status {
                        using Event = std::decay_t<decltype(concrete_event)>;
                        if constexpr (std::is_same_v<Event, LlmTextDeltaEvent>) {
                            output_text.append(concrete_event.text_delta);
                        }
                        return absl::OkStatus();
                    },
                    event);
            });

        if (!stream_status.ok()) {
            LOG(ERROR) << "LlmSleepCycleSemanticExtractor LLM call failed detail='"
                       << SanitizeForLog(stream_status.message()) << "'";
            return stream_status;
        }
        if (output_text.empty()) {
            LOG(WARNING) << "LlmSleepCycleSemanticExtractor LLM returned empty response";
            return invalid_argument("sleep-cycle semantic extractor LLM returned empty response");
        }

        absl::StatusOr<SleepCycleSemanticExtractionResult> parsed =
            ParseResponse(output_text, valid_episode_ids, valid_relationship_ids);
        if (!parsed.ok()) {
            LOG(WARNING) << "LlmSleepCycleSemanticExtractor failed to parse response detail='"
                         << SanitizeForLog(parsed.status().message()) << "'";
            return parsed.status();
        }
        return parsed;
    }

  private:
    std::shared_ptr<const LlmClient> llm_client_;
    std::string model_;
    std::string system_prompt_;
    isla::server::LlmReasoningEffort reasoning_effort_;
};

} // namespace

absl::StatusOr<SleepCycleSemanticExtractorPtr>
CreateLlmSleepCycleSemanticExtractor(std::shared_ptr<const isla::server::LlmClient> llm_client,
                                     std::string model,
                                     isla::server::LlmReasoningEffort reasoning_effort) {
    if (!llm_client) {
        return invalid_argument("LlmSleepCycleSemanticExtractor requires a non-null llm client");
    }
    if (model.empty()) {
        return invalid_argument("LlmSleepCycleSemanticExtractor requires a non-empty model name");
    }
    absl::StatusOr<std::string> system_prompt =
        LoadPrompt(PromptAsset::kSleepCycleSemanticExtractorSystemPrompt);
    if (!system_prompt.ok()) {
        return system_prompt.status();
    }
    return std::make_shared<LlmSleepCycleSemanticExtractor>(
        std::move(llm_client), std::move(model), std::move(*system_prompt), reasoning_effort);
}

} // namespace isla::server::memory
