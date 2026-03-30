#include "isla/server/memory/supabase_memory_store.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_json_client.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ExecuteHttpRequest;
using isla::server::HttpClientConfig;
using isla::server::HttpRequestSpec;
using isla::server::HttpResponse;
using isla::server::ParsedHttpUrl;
using isla::server::ParseHttpUrl;
using isla::server::PersistentHttpClient;
using isla::server::ai_gateway::SanitizeForLog;
using nlohmann::json;

using Clock = std::chrono::steady_clock;

constexpr std::string_view kEntityColumns =
    "entity_id,user_id,label,category,activeness,active_model_text,"
    "familiar_label_text,name_embedding,created_at,updated_at";

constexpr std::string_view kRelationshipColumns =
    "relationship_id,user_id,from_entity_id,predicate,to_entity_id,"
    "weight,observation_count,last_observed_at,source_episode_ids,"
    "embedding,is_archived,archived_at,superseded_by,created_at";

constexpr std::string_view kLongTermEpisodeColumns =
    "lte_id,user_id,summary_full,summary_compressed,keywords,embedding,"
    "outcome,complexity,original_episode_ids,caused_by,led_to,created_at";

json SerializeEmbeddingForVectorColumn(const Embedding& embedding) {
    if (embedding.empty()) {
        return nullptr;
    }
    return embedding;
}

json SerializeOptionalEmbeddingForVectorColumn(const std::optional<Embedding>& embedding) {
    if (!embedding.has_value() || embedding->empty()) {
        return nullptr;
    }
    return *embedding;
}

Embedding ParseEmbeddingJsonValueOrThrow(const json& value, std::string_view field_name,
                                         bool allow_null_as_empty) {
    if (value.is_null()) {
        if (allow_null_as_empty) {
            return {};
        }
        throw std::invalid_argument(std::string(field_name) + " must not be null");
    }

    Embedding embedding;
    if (value.is_array()) {
        embedding = value.get<Embedding>();
    } else if (value.is_string()) {
        json parsed;
        try {
            parsed = json::parse(value.get<std::string>());
        } catch (const json::parse_error& error) {
            throw std::invalid_argument(std::string(field_name) +
                                        " vector text contained invalid JSON: " + error.what());
        }
        if (!parsed.is_array()) {
            throw std::invalid_argument(std::string(field_name) +
                                        " vector text must decode to a JSON array");
        }
        embedding = parsed.get<Embedding>();
    } else {
        throw std::invalid_argument(std::string(field_name) +
                                    " must be a vector-compatible JSON array or string");
    }

    if (embedding.empty()) {
        if (allow_null_as_empty) {
            return {};
        }
        throw std::invalid_argument(std::string(field_name) + " must not be empty when set");
    }
    if (embedding.size() != kEmbeddingDimensions) {
        throw std::invalid_argument(std::string(field_name) + " must contain exactly " +
                                    std::to_string(kEmbeddingDimensions) + " elements");
    }
    return embedding;
}

std::optional<Embedding> ParseOptionalEmbeddingJsonValueOrThrow(const json& value,
                                                                std::string_view field_name) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return ParseEmbeddingJsonValueOrThrow(value, field_name, /*allow_null_as_empty=*/false);
}

json BuildSessionJson(const MemorySessionRecord& record) {
    return json{
        { "session_id", record.session_id },       { "user_id", record.user_id },
        { "system_prompt", record.system_prompt }, { "created_at", record.created_at },
        { "ended_at", record.ended_at },
    };
}

json BuildUserWorkingMemoryJson(const UserWorkingMemoryRecord& record) {
    return json{
        { "user_id", record.user_id },
        { "session_id", record.session_id },
        { "working_memory", record.working_memory },
        { "rendered_working_memory", record.rendered_working_memory },
        { "updated_at", record.updated_at },
    };
}

json BuildConversationItemJson(std::string_view session_id, std::int64_t item_index,
                               ConversationItemType item_type,
                               std::optional<std::string> episode_id,
                               std::optional<std::string> episode_stub_content,
                               std::optional<Timestamp> episode_stub_create_time) {
    return json{
        { "session_id", std::string(session_id) },
        { "item_index", item_index },
        { "item_type", item_type },
        { "episode_id", episode_id },
        { "episode_stub_content", episode_stub_content },
        { "episode_stub_created_at", episode_stub_create_time },
    };
}

json BuildConversationMessageJson(const ConversationMessageWrite& write) {
    return json{
        { "session_id", write.session_id },
        { "item_index", write.conversation_item_index },
        { "message_index", write.message_index },
        { "turn_id", write.turn_id },
        { "role", write.role },
        { "content", write.content },
        { "created_at", write.create_time },
    };
}

json BuildMidTermEpisodeJson(const MidTermEpisodeWrite& write) {
    return json{
        { "episode_id", write.episode.episode_id },
        { "session_id", write.session_id },
        { "source_item_index", write.source_conversation_item_index },
        { "tier1_detail", write.episode.tier1_detail },
        { "tier2_summary", write.episode.tier2_summary },
        { "tier3_ref", write.episode.tier3_ref },
        { "tier3_keywords", write.episode.tier3_keywords },
        { "salience", write.episode.salience },
        // NOTICE: Mid-term episodes now persist embeddings in pgvector. Keep in-memory
        // `Episode.embedding` empty when embeddings are disabled, but write SQL NULL so Postgres
        // does not receive an invalid zero-length vector literal.
        { "embedding", SerializeEmbeddingForVectorColumn(write.episode.embedding) },
        { "created_at", write.episode.created_at },
    };
}

json BuildEntityJson(const EntityWrite& write) {
    return json{
        { "entity_id", write.entity.entity_id },
        { "user_id", write.entity.user_id },
        { "label", write.entity.label },
        { "category", write.entity.category },
        { "activeness", write.entity.activeness },
        { "active_model_text", write.entity.active_model_text },
        { "familiar_label_text", write.entity.familiar_label_text },
        { "name_embedding",
          SerializeOptionalEmbeddingForVectorColumn(write.entity.name_embedding) },
        { "created_at", write.entity.created_at },
        { "updated_at", write.entity.updated_at },
    };
}

json BuildRelationshipJson(const RelationshipWrite& write) {
    return json{
        { "relationship_id", write.relationship.relationship_id },
        { "user_id", write.relationship.user_id },
        { "from_entity_id", write.relationship.from_entity_id },
        { "predicate", write.relationship.predicate },
        { "to_entity_id", write.relationship.to_entity_id },
        { "weight", write.relationship.weight },
        { "observation_count", write.relationship.observation_count },
        { "last_observed_at", write.relationship.last_observed_at },
        { "source_episode_ids", write.relationship.source_episode_ids },
        { "embedding", SerializeOptionalEmbeddingForVectorColumn(write.relationship.embedding) },
        { "is_archived", write.relationship.is_archived },
        { "archived_at", write.relationship.archived_at },
        { "superseded_by", write.relationship.superseded_by },
        { "created_at", write.relationship.created_at },
    };
}

json BuildLongTermEpisodeJson(const LongTermEpisodeWrite& write) {
    return json{
        { "lte_id", write.episode.lte_id },
        { "user_id", write.episode.user_id },
        { "summary_full", write.episode.summary_full },
        { "summary_compressed", write.episode.summary_compressed },
        { "keywords", write.episode.keywords },
        { "embedding", SerializeOptionalEmbeddingForVectorColumn(write.episode.embedding) },
        { "outcome", write.episode.outcome },
        { "complexity", write.episode.complexity },
        { "original_episode_ids", write.episode.original_episode_ids },
        { "caused_by", write.episode.caused_by },
        { "led_to", write.episode.led_to },
        { "created_at", write.episode.created_at },
    };
}

json BuildSleepCycleExtractionJson(const SleepCycleExtractionResult& result) {
    json entities = json::array();
    for (const EntityWrite& write : result.entities) {
        entities.push_back(BuildEntityJson(write));
    }

    json relationships = json::array();
    for (const RelationshipWrite& write : result.relationships) {
        relationships.push_back(BuildRelationshipJson(write));
    }

    json long_term_episodes = json::array();
    for (const LongTermEpisodeWrite& write : result.long_term_episodes) {
        long_term_episodes.push_back(BuildLongTermEpisodeJson(write));
    }

    json long_term_episode_entity_links = json::array();
    for (const LongTermEpisodeEntityLink& link : result.long_term_episode_entity_links) {
        long_term_episode_entity_links.push_back(json{
            { "lte_id", link.lte_id },
            { "entity_ids", link.entity_ids },
        });
    }

    return json{
        { "entities", std::move(entities) },
        { "relationships", std::move(relationships) },
        { "long_term_episodes", std::move(long_term_episodes) },
        { "long_term_episode_entity_links", std::move(long_term_episode_entity_links) },
    };
}

absl::StatusOr<Entity> ParseEntityRow(const json& row) {
    try {
        return Entity{
            .entity_id = row.at("entity_id").get<std::string>(),
            .user_id = row.at("user_id").get<std::string>(),
            .label = row.at("label").get<std::string>(),
            .category = row.at("category").get<std::string>(),
            .activeness = row.at("activeness").get<int>(),
            .active_model_text = row.at("active_model_text").get<std::optional<std::string>>(),
            .familiar_label_text = row.at("familiar_label_text").get<std::optional<std::string>>(),
            .name_embedding =
                ParseOptionalEmbeddingJsonValueOrThrow(row.at("name_embedding"), "name_embedding"),
            .created_at = row.at("created_at").get<Timestamp>(),
            .updated_at = row.at("updated_at").get<Timestamp>(),
        };
    } catch (const std::exception& error) {
        return absl::InternalError(std::string("supabase entities row was malformed: ") +
                                   error.what());
    }
}

absl::StatusOr<Relationship> ParseRelationshipRow(const json& row) {
    try {
        return Relationship{
            .relationship_id = row.at("relationship_id").get<std::string>(),
            .user_id = row.at("user_id").get<std::string>(),
            .from_entity_id = row.at("from_entity_id").get<std::string>(),
            .predicate = row.at("predicate").get<std::string>(),
            .to_entity_id = row.at("to_entity_id").get<std::string>(),
            .weight = row.at("weight").get<double>(),
            .observation_count = row.at("observation_count").get<int>(),
            .last_observed_at = row.at("last_observed_at").get<Timestamp>(),
            .source_episode_ids = row.at("source_episode_ids").get<std::vector<std::string>>(),
            .embedding = ParseOptionalEmbeddingJsonValueOrThrow(row.at("embedding"), "embedding"),
            .is_archived = row.at("is_archived").get<bool>(),
            .archived_at = row.at("archived_at").get<std::optional<Timestamp>>(),
            .superseded_by = row.at("superseded_by").get<std::optional<std::string>>(),
            .created_at = row.at("created_at").get<Timestamp>(),
        };
    } catch (const std::exception& error) {
        return absl::InternalError(std::string("supabase relationships row was malformed: ") +
                                   error.what());
    }
}

absl::StatusOr<LongTermEpisode> ParseLongTermEpisodeRow(const json& row) {
    try {
        return LongTermEpisode{
            .lte_id = row.at("lte_id").get<std::string>(),
            .user_id = row.at("user_id").get<std::string>(),
            .summary_full = row.at("summary_full").get<std::optional<std::string>>(),
            .summary_compressed = row.at("summary_compressed").get<std::string>(),
            .keywords = row.at("keywords").get<std::vector<std::string>>(),
            .embedding = ParseOptionalEmbeddingJsonValueOrThrow(row.at("embedding"), "embedding"),
            .outcome = row.at("outcome").get<LongTermEpisodeOutcome>(),
            .complexity = row.at("complexity").get<int>(),
            .created_at = row.at("created_at").get<Timestamp>(),
            .original_episode_ids = row.at("original_episode_ids").get<std::vector<std::string>>(),
            .caused_by = row.at("caused_by").get<std::optional<std::string>>(),
            .led_to = row.at("led_to").get<std::optional<std::string>>(),
        };
    } catch (const std::exception& error) {
        return absl::InternalError(std::string("supabase long_term_episodes row was malformed: ") +
                                   error.what());
    }
}

json BuildRemainingMessagesJson(const OngoingEpisode& episode) {
    json messages = json::array();
    for (const Message& message : episode.messages) {
        messages.push_back(json{
            { "role", message.role },
            { "content", message.content },
            { "created_at", message.create_time },
        });
    }
    return messages;
}

std::vector<std::pair<std::string, std::string>>
BuildSupabaseAuthHeaders(const SupabaseMemoryStoreConfig& config) {
    return {
        { "Authorization", "Bearer " + config.service_role_key },
        { "apikey", config.service_role_key },
        { "Accept", "application/json" },
    };
}

HttpRequestSpec BuildUpsertRequest(std::string_view table_name, const json& body,
                                   std::string on_conflict, std::string_view schema,
                                   const SupabaseMemoryStoreConfig& config) {
    HttpRequestSpec request{
        .method = boost::beast::http::verb::post,
        .target_path = "/rest/v1/" + std::string(table_name),
        .query_parameters = { { "on_conflict", std::move(on_conflict) } },
        .headers = BuildSupabaseAuthHeaders(config),
        .body = body.dump(),
    };
    request.headers.emplace_back("Prefer", "resolution=merge-duplicates,return=minimal");
    request.headers.emplace_back("Content-Profile", std::string(schema));
    return request;
}

HttpRequestSpec BuildGetRequest(std::string target_path,
                                std::vector<std::pair<std::string, std::string>> query_parameters,
                                std::string_view schema, const SupabaseMemoryStoreConfig& config) {
    HttpRequestSpec request{
        .method = boost::beast::http::verb::get,
        .target_path = std::move(target_path),
        .query_parameters = std::move(query_parameters),
        .headers = BuildSupabaseAuthHeaders(config),
        .body = std::nullopt,
    };
    request.headers.emplace_back("Accept-Profile", std::string(schema));
    return request;
}

HttpRequestSpec BuildRpcRequest(std::string_view function_name, const json& body,
                                std::string_view schema, const SupabaseMemoryStoreConfig& config) {
    HttpRequestSpec request{
        .method = boost::beast::http::verb::post,
        .target_path = "/rest/v1/rpc/" + std::string(function_name),
        .query_parameters = {},
        .headers = BuildSupabaseAuthHeaders(config),
        .body = body.dump(),
    };
    request.headers.emplace_back("Prefer", "return=minimal");
    request.headers.emplace_back("Content-Profile", std::string(schema));
    return request;
}

std::string ExtractSupabaseErrorDetail(std::string_view body) {
    if (body.empty()) {
        return "empty response body";
    }
    const json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_object()) {
        return std::string(body);
    }
    if (const auto it = parsed.find("message"); it != parsed.end() && it->is_string()) {
        return it->get<std::string>();
    }
    if (const auto it = parsed.find("error"); it != parsed.end()) {
        if (it->is_string()) {
            return it->get<std::string>();
        }
        if (it->is_object()) {
            if (const auto message_it = it->find("message");
                message_it != it->end() && message_it->is_string()) {
                return message_it->get<std::string>();
            }
        }
    }
    return std::string(body);
}

absl::Status MapSupabaseHttpError(unsigned int status_code, std::string_view body) {
    const std::string detail = ExtractSupabaseErrorDetail(body);
    switch (status_code) {
    case 400:
    case 422:
        return absl::InvalidArgumentError(detail);
    case 401:
    case 403:
        return absl::PermissionDeniedError(detail);
    case 404:
        return absl::NotFoundError(detail);
    case 409:
        return absl::FailedPreconditionError(detail);
    case 429:
        return absl::ResourceExhaustedError(detail);
    default:
        if (status_code >= 500U) {
            return absl::UnavailableError(detail);
        }
        return absl::UnknownError(detail);
    }
}

std::int64_t DurationMillis(Clock::time_point started_at, Clock::time_point completed_at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(completed_at - started_at).count();
}

std::string HttpVerbForLog(boost::beast::http::verb method) {
    return std::string(boost::beast::http::to_string(method));
}

void LogSupabaseRequestLatency(const SupabaseMemoryStoreConfig& config,
                               const HttpRequestSpec& request, Clock::time_point started_at,
                               Clock::time_point completed_at, std::string_view outcome) {
    if (!config.telemetry_logging_enabled) {
        return;
    }
    LOG(INFO) << "AI gateway supabase latency kind=request method="
              << SanitizeForLog(HttpVerbForLog(request.method))
              << " target=" << SanitizeForLog(request.target_path)
              << " duration_ms=" << DurationMillis(started_at, completed_at)
              << " outcome=" << SanitizeForLog(outcome);
}

void LogSupabaseOperationLatency(const SupabaseMemoryStoreConfig& config,
                                 std::string_view operation,
                                 std::optional<std::string_view> session_id,
                                 Clock::time_point started_at, Clock::time_point completed_at,
                                 std::string_view outcome) {
    if (!config.telemetry_logging_enabled) {
        return;
    }
    LOG(INFO) << "AI gateway supabase latency kind=operation op=" << SanitizeForLog(operation)
              << " session_id="
              << (session_id.has_value() ? SanitizeForLog(*session_id) : std::string("<none>"))
              << " duration_ms=" << DurationMillis(started_at, completed_at)
              << " outcome=" << SanitizeForLog(outcome);
}

class ScopedSupabaseOperationLatency final {
  public:
    ScopedSupabaseOperationLatency(const SupabaseMemoryStoreConfig& config,
                                   std::string_view operation,
                                   std::optional<std::string_view> session_id)
        : config_(config), operation_(operation),
          session_id_(session_id.has_value() ? std::optional<std::string>(*session_id)
                                             : std::nullopt),
          started_at_(Clock::now()) {}

    ScopedSupabaseOperationLatency(const ScopedSupabaseOperationLatency&) = delete;
    ScopedSupabaseOperationLatency& operator=(const ScopedSupabaseOperationLatency&) = delete;

    ~ScopedSupabaseOperationLatency() noexcept {
        LogSupabaseOperationLatency(config_, operation_, session_id_, started_at_, Clock::now(),
                                    outcome_);
    }

    void SetOutcome(std::string_view outcome) {
        outcome_ = std::string(outcome);
    }

  private:
    const SupabaseMemoryStoreConfig& config_;
    std::string operation_;
    std::optional<std::string> session_id_;
    Clock::time_point started_at_;
    std::string outcome_ = "error";
};

absl::StatusOr<std::string> ExecuteSupabaseRequest(PersistentHttpClient& client,
                                                   const SupabaseMemoryStoreConfig& config,
                                                   const HttpRequestSpec& request) {
    const Clock::time_point started_at = Clock::now();
    const absl::StatusOr<HttpResponse> response = client.Execute(request);
    if (!response.ok()) {
        const Clock::time_point completed_at = Clock::now();
        LogSupabaseRequestLatency(config, request, started_at, completed_at, "transport_error");
        return response.status();
    }
    if (response->status_code < 200U || response->status_code >= 300U) {
        const Clock::time_point completed_at = Clock::now();
        LogSupabaseRequestLatency(config, request, started_at, completed_at, "http_error");
        return MapSupabaseHttpError(response->status_code, response->body);
    }
    const Clock::time_point completed_at = Clock::now();
    LogSupabaseRequestLatency(config, request, started_at, completed_at, "ok");
    return response->body;
}

absl::StatusOr<json> ParseJsonArrayResponse(std::string_view body, std::string_view response_name) {
    json parsed;
    try {
        parsed = json::parse(body);
    } catch (const std::exception& error) {
        return absl::InternalError(std::string(response_name) +
                                   " response contained invalid JSON: " + error.what());
    }
    if (!parsed.is_array()) {
        return absl::InternalError(std::string(response_name) + " response must be an array");
    }
    return parsed;
}

absl::StatusOr<Episode> ParseEpisodeRow(const json& episode_json) {
    try {
        return Episode{
            .episode_id = episode_json.at("episode_id").get<std::string>(),
            .tier1_detail = episode_json.at("tier1_detail").get<std::optional<std::string>>(),
            .tier2_summary = episode_json.at("tier2_summary").get<std::string>(),
            .tier3_ref = episode_json.at("tier3_ref").get<std::string>(),
            .tier3_keywords = episode_json.at("tier3_keywords").get<std::vector<std::string>>(),
            .salience = episode_json.at("salience").get<int>(),
            .embedding =
                ParseEmbeddingJsonValueOrThrow(episode_json.at("embedding"), "embedding", true),
            .created_at = episode_json.at("created_at").get<Timestamp>(),
        };
    } catch (const std::exception& error) {
        return absl::InternalError(std::string("supabase mid_term_episodes row was malformed: ") +
                                   error.what());
    }
}

class SupabaseMemoryStore final : public MemoryStore {
  public:
    SupabaseMemoryStore(SupabaseMemoryStoreConfig config,
                        std::unique_ptr<PersistentHttpClient> client)
        : config_(std::move(config)), client_(std::move(client)) {}

    [[nodiscard]] absl::Status WarmUp() override {
        return client_->WarmUp();
    }

    [[nodiscard]] absl::Status UpsertSession(const MemorySessionRecord& record) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_session", record.session_id);
        if (absl::Status status = ValidateMemorySessionRecord(record); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("memory_sessions", json::array({ BuildSessionJson(record) }),
                               "session_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    UpsertUserWorkingMemory(const UserWorkingMemoryRecord& record) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_user_working_memory",
                                               record.session_id);
        if (absl::Status status = ValidateUserWorkingMemoryRecord(record); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request = BuildUpsertRequest(
            "user_working_memory", json::array({ BuildUserWorkingMemoryJson(record) }), "user_id",
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    AppendConversationMessage(const ConversationMessageWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "append_conversation_message",
                                               write.session_id);
        if (absl::Status status = ValidateConversationMessageWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }

        const HttpRequestSpec item_request = BuildUpsertRequest(
            "conversation_items",
            json::array({ BuildConversationItemJson(write.session_id, write.conversation_item_index,
                                                    ConversationItemType::OngoingEpisode,
                                                    std::nullopt, std::nullopt, std::nullopt) }),
            "session_id,item_index", config_.schema, config_);
        const absl::StatusOr<std::string> item_response =
            ExecuteSupabaseRequest(*client_, config_, item_request);
        if (!item_response.ok()) {
            return item_response.status();
        }

        const HttpRequestSpec message_request = BuildUpsertRequest(
            "conversation_messages", json::array({ BuildConversationMessageJson(write) }),
            "session_id,item_index,message_index", config_.schema, config_);
        const absl::StatusOr<std::string> message_response =
            ExecuteSupabaseRequest(*client_, config_, message_request);
        if (!message_response.ok()) {
            return message_response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    ReplaceConversationItemWithEpisodeStub(const EpisodeStubWrite& write) override {
        ScopedSupabaseOperationLatency latency(
            config_, "replace_conversation_item_with_episode_stub", write.session_id);
        if (absl::Status status = ValidateEpisodeStubWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request = BuildUpsertRequest(
            "conversation_items",
            json::array({ BuildConversationItemJson(
                write.session_id, write.conversation_item_index, ConversationItemType::EpisodeStub,
                write.episode_id, write.episode_stub_content, write.episode_stub_create_time) }),
            "session_id,item_index", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    SplitConversationItemWithEpisodeStub(const SplitEpisodeStubWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "split_conversation_item_with_episode_stub",
                                               write.session_id);
        if (absl::Status status = ValidateSplitEpisodeStubWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec split_flush_request =
            BuildRpcRequest("split_conversation_item_with_episode_stub",
                            json{
                                { "p_session_id", write.session_id },
                                { "p_conversation_item_index", write.conversation_item_index },
                                { "p_episode_id", write.episode_id },
                                { "p_episode_stub_content", write.episode_stub_content },
                                { "p_episode_stub_created_at", write.episode_stub_create_time },
                                { "p_remaining_messages",
                                  BuildRemainingMessagesJson(write.remaining_ongoing_episode) },
                            },
                            config_.schema, config_);
        const absl::StatusOr<std::string> split_flush_response =
            ExecuteSupabaseRequest(*client_, config_, split_flush_request);
        if (!split_flush_response.ok()) {
            return split_flush_response.status();
        }

        VLOG(1) << "SupabaseMemoryStore persisted split episode stub item via RPC session_id="
                << SanitizeForLog(write.session_id)
                << " conversation_item_index=" << write.conversation_item_index
                << " inserted_item_index=" << (write.conversation_item_index + 1)
                << " remaining_message_count=" << write.remaining_ongoing_episode.messages.size();
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status ClearSessionWorkingSet(std::string_view session_id) override {
        ScopedSupabaseOperationLatency latency(config_, "clear_session_working_set", session_id);
        if (session_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "ClearSessionWorkingSet requires session_id to be non-empty");
        }

        const HttpRequestSpec request = BuildRpcRequest(
            "clear_session_working_set", json{ { "p_session_id", std::string(session_id) } },
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status UpsertMidTermEpisode(const MidTermEpisodeWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_mid_term_episode",
                                               write.session_id);
        if (absl::Status status = ValidateMidTermEpisodeWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("mid_term_episodes", json::array({ BuildMidTermEpisodeJson(write) }),
                               "episode_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<std::vector<Episode>>
    ListMidTermEpisodes(std::string_view session_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "list_mid_term_episodes", session_id);
        if (session_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "ListMidTermEpisodes requires session_id to be non-empty");
        }

        const HttpRequestSpec request = BuildGetRequest(
            "/rest/v1/mid_term_episodes",
            {
                { "select", "episode_id,tier1_detail,tier2_summary,tier3_ref,tier3_keywords,"
                            "salience,embedding,created_at" },
                { "session_id", "eq." + std::string(session_id) },
                { "order", "created_at.asc" },
            },
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> rows =
            ParseJsonArrayResponse(*response, "supabase mid_term_episodes");
        if (!rows.ok()) {
            return rows.status();
        }

        std::vector<Episode> episodes;
        episodes.reserve(rows->size());
        for (const json& row : *rows) {
            absl::StatusOr<Episode> episode = ParseEpisodeRow(row);
            if (!episode.ok()) {
                return episode.status();
            }
            episodes.push_back(std::move(*episode));
        }
        latency.SetOutcome("ok");
        return episodes;
    }

    [[nodiscard]] absl::StatusOr<std::optional<Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "get_mid_term_episode", session_id);
        if (session_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "GetMidTermEpisode requires session_id to be non-empty");
        }
        if (episode_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "GetMidTermEpisode requires episode_id to be non-empty");
        }

        const HttpRequestSpec request = BuildGetRequest(
            "/rest/v1/mid_term_episodes",
            {
                { "select", "episode_id,tier1_detail,tier2_summary,tier3_ref,tier3_keywords,"
                            "salience,embedding,created_at" },
                { "session_id", "eq." + std::string(session_id) },
                { "episode_id", "eq." + std::string(episode_id) },
                { "limit", "2" },
            },
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> rows =
            ParseJsonArrayResponse(*response, "supabase mid_term_episodes");
        if (!rows.ok()) {
            return rows.status();
        }
        if (rows->empty()) {
            latency.SetOutcome("not_found");
            return std::nullopt;
        }
        if (rows->size() != 1U) {
            return absl::InternalError(
                "supabase mid_term_episodes response returned multiple rows for one episode");
        }

        absl::StatusOr<Episode> episode = ParseEpisodeRow(rows->front());
        if (!episode.ok()) {
            return episode.status();
        }
        latency.SetOutcome("ok");
        return std::optional<Episode>(std::move(*episode));
    }

    [[nodiscard]] absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "load_snapshot", session_id);
        if (session_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError("LoadSnapshot requires session_id to be non-empty");
        }

        const HttpRequestSpec session_request = BuildGetRequest(
            "/rest/v1/memory_sessions",
            {
                { "select", "session_id,user_id,system_prompt,created_at,ended_at" },
                { "session_id", "eq." + std::string(session_id) },
            },
            config_.schema, config_);
        const absl::StatusOr<std::string> session_response =
            ExecuteSupabaseRequest(*client_, config_, session_request);
        if (!session_response.ok()) {
            return session_response.status();
        }
        const absl::StatusOr<json> session_rows =
            ParseJsonArrayResponse(*session_response, "supabase memory_sessions");
        if (!session_rows.ok()) {
            return session_rows.status();
        }
        if (session_rows->empty()) {
            latency.SetOutcome("not_found");
            return std::nullopt;
        }
        if (session_rows->size() != 1U) {
            return absl::InternalError("supabase memory_sessions response returned multiple rows");
        }

        MemoryStoreSnapshot snapshot;
        try {
            const json& session_json = session_rows->front();
            snapshot.session = MemorySessionRecord{
                .session_id = session_json.at("session_id").get<std::string>(),
                .user_id = session_json.at("user_id").get<std::string>(),
                .system_prompt = session_json.at("system_prompt").get<std::string>(),
                .created_at = session_json.at("created_at").get<Timestamp>(),
                .ended_at =
                    session_json.at("ended_at").is_null()
                        ? std::nullopt
                        : std::optional<Timestamp>(session_json.at("ended_at").get<Timestamp>()),
            };
        } catch (const std::exception& error) {
            return absl::InternalError(std::string("supabase memory_sessions row was malformed: ") +
                                       error.what());
        }

        const std::string session_id_string(session_id);
        const auto fetch_item_rows = [this, session_id_string]() -> absl::StatusOr<json> {
            const HttpRequestSpec items_request =
                BuildGetRequest("/rest/v1/conversation_items",
                                {
                                    { "select", "item_index,item_type,episode_id,episode_stub_"
                                                "content,episode_stub_created_at" },
                                    { "session_id", "eq." + session_id_string },
                                    { "order", "item_index.asc" },
                                },
                                config_.schema, config_);
            const absl::StatusOr<std::string> items_response =
                ExecuteSupabaseRequest(*client_, config_, items_request);
            if (!items_response.ok()) {
                return items_response.status();
            }
            return ParseJsonArrayResponse(*items_response, "supabase conversation_items");
        };
        const auto fetch_message_rows = [this, session_id_string]() -> absl::StatusOr<json> {
            const HttpRequestSpec messages_request =
                BuildGetRequest("/rest/v1/conversation_messages",
                                {
                                    { "select", "item_index,message_index,role,content,created_at,"
                                                "conversation_items!inner(item_type)" },
                                    { "session_id", "eq." + session_id_string },
                                    { "conversation_items.item_type", "eq.ongoing_episode" },
                                    { "order", "item_index.asc,message_index.asc" },
                                },
                                config_.schema, config_);
            const absl::StatusOr<std::string> messages_response =
                ExecuteSupabaseRequest(*client_, config_, messages_request);
            if (!messages_response.ok()) {
                return messages_response.status();
            }
            return ParseJsonArrayResponse(*messages_response, "supabase conversation_messages");
        };
        const auto fetch_episode_rows = [this, session_id_string]() -> absl::StatusOr<json> {
            const HttpRequestSpec episodes_request =
                BuildGetRequest("/rest/v1/mid_term_episodes",
                                {
                                    { "select", "episode_id,tier1_detail,tier2_summary,tier3_ref,"
                                                "tier3_keywords,salience,embedding,created_at" },
                                    { "session_id", "eq." + session_id_string },
                                    { "order", "created_at.asc" },
                                },
                                config_.schema, config_);
            const absl::StatusOr<std::string> episodes_response =
                ExecuteSupabaseRequest(*client_, config_, episodes_request);
            if (!episodes_response.ok()) {
                return episodes_response.status();
            }
            return ParseJsonArrayResponse(*episodes_response, "supabase mid_term_episodes");
        };

        // These run sequentially on the same persistent connection. Using
        // std::async would only add thread overhead since PersistentHttpClient
        // serializes requests via its internal mutex.
        const absl::StatusOr<json> item_rows = fetch_item_rows();
        if (!item_rows.ok()) {
            return item_rows.status();
        }
        try {
            for (const json& item_json : *item_rows) {
                PersistedConversationItem item;
                item.conversation_item_index = item_json.at("item_index").get<std::int64_t>();
                item.type = item_json.at("item_type").get<ConversationItemType>();
                if (item.type == ConversationItemType::OngoingEpisode) {
                    item.ongoing_episode = OngoingEpisode{ .messages = {} };
                } else {
                    item.episode_stub = EpisodeStub{
                        .content = item_json.at("episode_stub_content").get<std::string>(),
                        .create_time = item_json.at("episode_stub_created_at").get<Timestamp>(),
                    };
                    item.episode_id = item_json.at("episode_id").get<std::string>();
                }
                snapshot.conversation_items.push_back(std::move(item));
            }
        } catch (const std::exception& error) {
            return absl::InternalError(
                std::string("supabase conversation_items row was malformed: ") + error.what());
        }

        const absl::StatusOr<json> message_rows = fetch_message_rows();
        if (!message_rows.ok()) {
            return message_rows.status();
        }
        try {
            for (const json& message_json : *message_rows) {
                const std::int64_t item_index = message_json.at("item_index").get<std::int64_t>();
                if (item_index < 0 ||
                    static_cast<std::size_t>(item_index) >= snapshot.conversation_items.size() ||
                    snapshot.conversation_items[static_cast<std::size_t>(item_index)]
                            .conversation_item_index != item_index) {
                    return absl::InternalError("supabase conversation_messages row referenced an "
                                               "unknown conversation item");
                }
                PersistedConversationItem& item =
                    snapshot.conversation_items[static_cast<std::size_t>(item_index)];
                if (!item.ongoing_episode.has_value()) {
                    // Keep archived transcript rows for episode stubs in storage, but only
                    // hydrate messages that still belong to a live ongoing episode.
                    continue;
                }
                item.ongoing_episode->messages.push_back(Message{
                    .role = message_json.at("role").get<MessageRole>(),
                    .content = message_json.at("content").get<std::string>(),
                    .create_time = message_json.at("created_at").get<Timestamp>(),
                });
            }
        } catch (const std::exception& error) {
            return absl::InternalError(
                std::string("supabase conversation_messages row was malformed: ") + error.what());
        }

        const absl::StatusOr<json> episode_rows = fetch_episode_rows();
        if (!episode_rows.ok()) {
            return episode_rows.status();
        }
        for (const json& episode_json : *episode_rows) {
            absl::StatusOr<Episode> episode = ParseEpisodeRow(episode_json);
            if (!episode.ok()) {
                return episode.status();
            }
            snapshot.mid_term_episodes.push_back(std::move(*episode));
        }

        if (absl::Status status = ValidateMemoryStoreSnapshot(snapshot); !status.ok()) {
            return status;
        }
        latency.SetOutcome("ok");
        return snapshot;
    }

    // -------------------------------------------------------------------------
    // Long-term memory (Knowledge Graph + Episodic Vector Store)
    // -------------------------------------------------------------------------

    [[nodiscard]] absl::Status
    PersistSleepCycleExtraction(const SleepCycleExtractionResult& result) override {
        ScopedSupabaseOperationLatency latency(config_, "persist_sleep_cycle_extraction",
                                               std::nullopt);
        if (absl::Status status = ValidateSleepCycleExtractionResult(result); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request =
            BuildRpcRequest("persist_sleep_cycle_extraction",
                            json{
                                { "p_extraction", BuildSleepCycleExtractionJson(result) },
                            },
                            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status UpsertEntity(const EntityWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_entity", std::nullopt);
        if (absl::Status status = ValidateEntityWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("entities", json::array({ BuildEntityJson(write) }), "entity_id",
                               config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status UpsertRelationship(const RelationshipWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_relationship", std::nullopt);
        if (absl::Status status = ValidateRelationshipWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("relationships", json::array({ BuildRelationshipJson(write) }),
                               "relationship_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status UpsertLongTermEpisode(const LongTermEpisodeWrite& write) override {
        ScopedSupabaseOperationLatency latency(config_, "upsert_long_term_episode", std::nullopt);
        if (absl::Status status = ValidateLongTermEpisodeWrite(write); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        const HttpRequestSpec request = BuildUpsertRequest(
            "long_term_episodes", json::array({ BuildLongTermEpisodeJson(write) }), "lte_id",
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    LinkLongTermEpisodeEntities(const LongTermEpisodeEntityLink& link) override {
        ScopedSupabaseOperationLatency latency(config_, "link_long_term_episode_entities",
                                               std::nullopt);
        if (absl::Status status = ValidateLongTermEpisodeEntityLink(link); !status.ok()) {
            latency.SetOutcome("validation_error");
            return status;
        }
        json rows = json::array();
        for (const std::string& entity_id : link.entity_ids) {
            rows.push_back(json{
                { "lte_id", link.lte_id },
                { "entity_id", entity_id },
            });
        }
        const HttpRequestSpec request = BuildUpsertRequest(
            "long_term_episode_entities", rows, "lte_id,entity_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        latency.SetOutcome("ok");
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<std::vector<Entity>>
    ListEntitiesByUser(std::string_view user_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "list_entities_by_user", std::nullopt);
        if (user_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "ListEntitiesByUser requires user_id to be non-empty");
        }

        const HttpRequestSpec request =
            BuildGetRequest("/rest/v1/entities",
                            {
                                { "select", std::string(kEntityColumns) },
                                { "user_id", "eq." + std::string(user_id) },
                                { "order", "created_at.asc" },
                            },
                            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> rows = ParseJsonArrayResponse(*response, "supabase entities");
        if (!rows.ok()) {
            return rows.status();
        }

        std::vector<Entity> entities;
        entities.reserve(rows->size());
        for (const json& row : *rows) {
            absl::StatusOr<Entity> entity = ParseEntityRow(row);
            if (!entity.ok()) {
                return entity.status();
            }
            entities.push_back(std::move(*entity));
        }
        latency.SetOutcome("ok");
        return entities;
    }

    [[nodiscard]] absl::StatusOr<std::optional<Entity>>
    GetEntity(std::string_view entity_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "get_entity", std::nullopt);
        if (entity_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError("GetEntity requires entity_id to be non-empty");
        }

        const HttpRequestSpec request =
            BuildGetRequest("/rest/v1/entities",
                            {
                                { "select", std::string(kEntityColumns) },
                                { "entity_id", "eq." + std::string(entity_id) },
                                { "limit", "2" },
                            },
                            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> rows = ParseJsonArrayResponse(*response, "supabase entities");
        if (!rows.ok()) {
            return rows.status();
        }
        if (rows->empty()) {
            latency.SetOutcome("not_found");
            return std::nullopt;
        }
        if (rows->size() != 1U) {
            return absl::InternalError(
                "supabase entities response returned multiple rows for one entity");
        }

        absl::StatusOr<Entity> entity = ParseEntityRow(rows->front());
        if (!entity.ok()) {
            return entity.status();
        }
        latency.SetOutcome("ok");
        return std::optional<Entity>(std::move(*entity));
    }

    [[nodiscard]] absl::StatusOr<std::vector<Relationship>>
    ListRelationshipsForEntity(std::string_view entity_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "list_relationships_for_entity",
                                               std::nullopt);
        if (entity_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "ListRelationshipsForEntity requires entity_id to be non-empty");
        }

        const HttpRequestSpec request =
            BuildGetRequest("/rest/v1/relationships",
                            {
                                { "select", std::string(kRelationshipColumns) },
                                { "from_entity_id", "eq." + std::string(entity_id) },
                                { "is_archived", "eq.false" },
                                { "order", "weight.desc" },
                            },
                            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> rows =
            ParseJsonArrayResponse(*response, "supabase relationships");
        if (!rows.ok()) {
            return rows.status();
        }

        std::vector<Relationship> relationships;
        relationships.reserve(rows->size());
        for (const json& row : *rows) {
            absl::StatusOr<Relationship> rel = ParseRelationshipRow(row);
            if (!rel.ok()) {
                return rel.status();
            }
            relationships.push_back(std::move(*rel));
        }
        latency.SetOutcome("ok");
        return relationships;
    }

    [[nodiscard]] absl::StatusOr<std::vector<LongTermEpisode>>
    ListLongTermEpisodesForEntity(std::string_view entity_id) const override {
        ScopedSupabaseOperationLatency latency(config_, "list_long_term_episodes_for_entity",
                                               std::nullopt);
        if (entity_id.empty()) {
            latency.SetOutcome("validation_error");
            return absl::InvalidArgumentError(
                "ListLongTermEpisodesForEntity requires entity_id to be non-empty");
        }

        // Use resource embedding to fetch episodes in a single request.
        const HttpRequestSpec request = BuildGetRequest(
            "/rest/v1/long_term_episode_entities",
            {
                { "select", "long_term_episodes(" + std::string(kLongTermEpisodeColumns) + ")" },
                { "entity_id", "eq." + std::string(entity_id) },
                { "order", "long_term_episodes.created_at.asc" },
            },
            config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(*client_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        const absl::StatusOr<json> junction_rows =
            ParseJsonArrayResponse(*response, "supabase long_term_episode_entities");
        if (!junction_rows.ok()) {
            return junction_rows.status();
        }
        if (junction_rows->empty()) {
            latency.SetOutcome("ok");
            return std::vector<LongTermEpisode>{};
        }

        std::vector<LongTermEpisode> episodes;
        episodes.reserve(junction_rows->size());
        for (const json& row : *junction_rows) {
            if (!row.contains("long_term_episodes") || row.at("long_term_episodes").is_null()) {
                continue;
            }
            absl::StatusOr<LongTermEpisode> episode =
                ParseLongTermEpisodeRow(row.at("long_term_episodes"));
            if (!episode.ok()) {
                return episode.status();
            }
            episodes.push_back(std::move(*episode));
        }
        latency.SetOutcome("ok");
        return episodes;
    }

  private:
    SupabaseMemoryStoreConfig config_;
    std::unique_ptr<PersistentHttpClient> client_;
};

} // namespace

absl::Status ValidateSupabaseMemoryStoreConfig(const SupabaseMemoryStoreConfig& config) {
    if (!config.enabled) {
        return absl::OkStatus();
    }
    if (config.url.empty()) {
        return absl::InvalidArgumentError(
            "supabase url must not be empty when the store is enabled");
    }
    if (config.service_role_key.empty()) {
        return absl::InvalidArgumentError(
            "supabase service_role_key must not be empty when the store is enabled");
    }
    if (config.schema.empty()) {
        return absl::InvalidArgumentError(
            "supabase schema must not be empty when the store is enabled");
    }
    if (config.request_timeout <= std::chrono::milliseconds::zero()) {
        return absl::InvalidArgumentError("supabase request_timeout must be positive");
    }
    return absl::OkStatus();
}

absl::StatusOr<MemoryStorePtr> CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig config) {
    if (!config.enabled) {
        return MemoryStorePtr{};
    }
    if (absl::Status status = ValidateSupabaseMemoryStoreConfig(config); !status.ok()) {
        return status;
    }
    const absl::StatusOr<ParsedHttpUrl> parsed_url = ParseHttpUrl(config.url, "supabase url");
    if (!parsed_url.ok()) {
        return parsed_url.status();
    }
#if defined(_WIN32)
    if (parsed_url->scheme == "https") {
        return absl::FailedPreconditionError("supabase https transport is unavailable in Windows "
                                             "builds; run the gateway server on Linux");
    }
#endif
    const HttpClientConfig http_config{
        .request_timeout = config.request_timeout,
        .user_agent = config.user_agent,
        .trusted_ca_cert_pem = config.trusted_ca_cert_pem,
    };
    auto client = std::make_unique<PersistentHttpClient>(*parsed_url, http_config);
    return std::make_shared<SupabaseMemoryStore>(std::move(config), std::move(client));
}

} // namespace isla::server::memory
