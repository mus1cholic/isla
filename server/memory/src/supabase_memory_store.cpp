#include "isla/server/memory/supabase_memory_store.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_json_client.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ExecuteHttpRequest;
using isla::server::HttpClientConfig;
using isla::server::HttpRequestSpec;
using isla::server::HttpResponse;
using isla::server::ParsedHttpUrl;
using isla::server::ParseHttpUrl;
using nlohmann::json;

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

absl::StatusOr<std::string> ExecuteSupabaseRequest(const ParsedHttpUrl& parsed_url,
                                                   const SupabaseMemoryStoreConfig& config,
                                                   const HttpRequestSpec& request) {
    const HttpClientConfig http_config{
        .request_timeout = config.request_timeout,
        .user_agent = config.user_agent,
        .trusted_ca_cert_pem = config.trusted_ca_cert_pem,
    };
    const absl::StatusOr<HttpResponse> response =
        ExecuteHttpRequest(parsed_url, http_config, request);
    if (!response.ok()) {
        return response.status();
    }
    if (response->status_code < 200U || response->status_code >= 300U) {
        return MapSupabaseHttpError(response->status_code, response->body);
    }
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

class SupabaseMemoryStore final : public MemoryStore {
  public:
    SupabaseMemoryStore(SupabaseMemoryStoreConfig config, ParsedHttpUrl parsed_url)
        : config_(std::move(config)), parsed_url_(std::move(parsed_url)) {}

    [[nodiscard]] absl::Status UpsertSession(const MemorySessionRecord& record) override {
        if (absl::Status status = ValidateMemorySessionRecord(record); !status.ok()) {
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("memory_sessions", json::array({ BuildSessionJson(record) }),
                               "session_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(parsed_url_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    AppendConversationMessage(const ConversationMessageWrite& write) override {
        if (absl::Status status = ValidateConversationMessageWrite(write); !status.ok()) {
            return status;
        }

        const HttpRequestSpec item_request = BuildUpsertRequest(
            "conversation_items",
            json::array({ BuildConversationItemJson(write.session_id, write.conversation_item_index,
                                                    ConversationItemType::OngoingEpisode,
                                                    std::nullopt, std::nullopt, std::nullopt) }),
            "session_id,item_index", config_.schema, config_);
        const absl::StatusOr<std::string> item_response =
            ExecuteSupabaseRequest(parsed_url_, config_, item_request);
        if (!item_response.ok()) {
            return item_response.status();
        }

        const HttpRequestSpec message_request = BuildUpsertRequest(
            "conversation_messages", json::array({ BuildConversationMessageJson(write) }),
            "session_id,item_index,message_index", config_.schema, config_);
        const absl::StatusOr<std::string> message_response =
            ExecuteSupabaseRequest(parsed_url_, config_, message_request);
        if (!message_response.ok()) {
            return message_response.status();
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    ReplaceConversationItemWithEpisodeStub(const EpisodeStubWrite& write) override {
        if (absl::Status status = ValidateEpisodeStubWrite(write); !status.ok()) {
            return status;
        }
        const HttpRequestSpec request = BuildUpsertRequest(
            "conversation_items",
            json::array({ BuildConversationItemJson(
                write.session_id, write.conversation_item_index, ConversationItemType::EpisodeStub,
                write.episode_id, write.episode_stub_content, write.episode_stub_create_time) }),
            "session_id,item_index", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(parsed_url_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status UpsertMidTermEpisode(const MidTermEpisodeWrite& write) override {
        if (absl::Status status = ValidateMidTermEpisodeWrite(write); !status.ok()) {
            return status;
        }
        const HttpRequestSpec request =
            BuildUpsertRequest("mid_term_episodes", json::array({ BuildMidTermEpisodeJson(write) }),
                               "episode_id", config_.schema, config_);
        const absl::StatusOr<std::string> response =
            ExecuteSupabaseRequest(parsed_url_, config_, request);
        if (!response.ok()) {
            return response.status();
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<std::optional<MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        if (session_id.empty()) {
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
            ExecuteSupabaseRequest(parsed_url_, config_, session_request);
        if (!session_response.ok()) {
            return session_response.status();
        }
        const absl::StatusOr<json> session_rows =
            ParseJsonArrayResponse(*session_response, "supabase memory_sessions");
        if (!session_rows.ok()) {
            return session_rows.status();
        }
        if (session_rows->empty()) {
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
                ExecuteSupabaseRequest(parsed_url_, config_, items_request);
            if (!items_response.ok()) {
                return items_response.status();
            }
            return ParseJsonArrayResponse(*items_response, "supabase conversation_items");
        };
        const auto fetch_message_rows = [this, session_id_string]() -> absl::StatusOr<json> {
            const HttpRequestSpec messages_request = BuildGetRequest(
                "/rest/v1/conversation_messages",
                {
                    { "select", "item_index,message_index,role,content,created_at" },
                    { "session_id", "eq." + session_id_string },
                    { "order", "item_index.asc,message_index.asc" },
                },
                config_.schema, config_);
            const absl::StatusOr<std::string> messages_response =
                ExecuteSupabaseRequest(parsed_url_, config_, messages_request);
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
                ExecuteSupabaseRequest(parsed_url_, config_, episodes_request);
            if (!episodes_response.ok()) {
                return episodes_response.status();
            }
            return ParseJsonArrayResponse(*episodes_response, "supabase mid_term_episodes");
        };

        auto item_rows_future = std::async(std::launch::async, fetch_item_rows);
        auto message_rows_future = std::async(std::launch::async, fetch_message_rows);
        auto episode_rows_future = std::async(std::launch::async, fetch_episode_rows);

        const absl::StatusOr<json> item_rows = item_rows_future.get();
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

        const absl::StatusOr<json> message_rows = message_rows_future.get();
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
                    return absl::InternalError("supabase conversation_messages row referenced a "
                                               "non-ongoing conversation item");
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

        const absl::StatusOr<json> episode_rows = episode_rows_future.get();
        if (!episode_rows.ok()) {
            return episode_rows.status();
        }
        try {
            for (const json& episode_json : *episode_rows) {
                snapshot.mid_term_episodes.push_back(Episode{
                    .episode_id = episode_json.at("episode_id").get<std::string>(),
                    .tier1_detail = episode_json.at("tier1_detail").is_null()
                                        ? std::nullopt
                                        : std::optional<std::string>(
                                              episode_json.at("tier1_detail").get<std::string>()),
                    .tier2_summary = episode_json.at("tier2_summary").get<std::string>(),
                    .tier3_ref = episode_json.at("tier3_ref").get<std::string>(),
                    .tier3_keywords =
                        episode_json.at("tier3_keywords").get<std::vector<std::string>>(),
                    .salience = episode_json.at("salience").get<int>(),
                    .embedding = episode_json.at("embedding").get<Embedding>(),
                    .created_at = episode_json.at("created_at").get<Timestamp>(),
                });
            }
        } catch (const std::exception& error) {
            return absl::InternalError(
                std::string("supabase mid_term_episodes row was malformed: ") + error.what());
        }

        if (absl::Status status = ValidateMemoryStoreSnapshot(snapshot); !status.ok()) {
            return status;
        }
        return snapshot;
    }

  private:
    SupabaseMemoryStoreConfig config_;
    ParsedHttpUrl parsed_url_;
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
    return std::make_shared<SupabaseMemoryStore>(std::move(config), *parsed_url);
}

} // namespace isla::server::memory
