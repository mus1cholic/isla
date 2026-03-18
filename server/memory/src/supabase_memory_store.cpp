#include "isla/server/memory/supabase_memory_store.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
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

json BuildSessionJson(const MemorySessionRecord& record) {
    return json{
        { "session_id", record.session_id },       { "user_id", record.user_id },
        { "system_prompt", record.system_prompt }, { "created_at", record.created_at },
        { "ended_at", record.ended_at },
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
        { "embedding", write.episode.embedding },
        { "created_at", write.episode.created_at },
    };
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
            .embedding = episode_json.at("embedding").get<Embedding>(),
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
