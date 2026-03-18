#include "isla/server/memory/mid_term_compactor.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "isla/server/openai_responses_client.hpp"
#include "nlohmann/json.hpp"

namespace isla::server::memory {
namespace {

using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesEvent;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::ai_gateway::SanitizeForLog;
using nlohmann::json;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(message);
}

json SerializeEpisodeForCompactor(const OngoingEpisode& episode) {
    json messages_json = json::array();
    for (const Message& message : episode.messages) {
        messages_json.push_back({
            { "role", message.role == MessageRole::User ? "user" : "assistant" },
            { "content", message.content },
        });
    }
    return json{ { "messages", std::move(messages_json) } };
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
        const auto it = std::find(keys.begin(), keys.end(), key);
        if (it == keys.end()) {
            return invalid_argument(std::string(context) + " contains unexpected field '" + key +
                                    "'");
        }
    }
    for (const std::string_view key : keys) {
        if (!object.contains(key)) {
            return invalid_argument(std::string(context) + " is missing required field '" +
                                    std::string(key) + "'");
        }
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> ParseRequiredNonEmptyString(const json& object, std::string_view field,
                                                        std::string_view context) {
    if (!object.at(field).is_string()) {
        return invalid_argument(std::string(context) + " field '" + std::string(field) +
                                "' must be a string");
    }
    const std::string value = object.at(field).get<std::string>();
    if (value.empty()) {
        return invalid_argument(std::string(context) + " field '" + std::string(field) +
                                "' must not be empty");
    }
    return value;
}

absl::StatusOr<CompactedMidTermEpisode> ParseCompactorResponse(const std::string& response_text) {
    json response;
    try {
        response = json::parse(response_text);
    } catch (const json::parse_error& error) {
        return invalid_argument(std::string("mid-term compactor returned invalid JSON: ") +
                                error.what());
    }

    constexpr std::array<std::string_view, 5> kExpectedFields = {
        "tier1_detail", "tier2_summary", "tier3_ref", "tier3_keywords", "salience",
    };
    if (absl::Status status =
            ValidateExactObjectKeys(response, kExpectedFields, "mid-term compactor response");
        !status.ok()) {
        return status;
    }

    const json& tier1_detail_json = response.at("tier1_detail");
    std::optional<std::string> tier1_detail;
    if (tier1_detail_json.is_null()) {
        tier1_detail = std::nullopt;
    } else if (tier1_detail_json.is_string()) {
        std::string detail = tier1_detail_json.get<std::string>();
        if (detail.empty()) {
            return invalid_argument(
                "mid-term compactor response field 'tier1_detail' must not be empty when set");
        }
        tier1_detail = std::move(detail);
    } else {
        return invalid_argument(
            "mid-term compactor response field 'tier1_detail' must be a string or null");
    }

    absl::StatusOr<std::string> tier2_summary =
        ParseRequiredNonEmptyString(response, "tier2_summary", "mid-term compactor response");
    if (!tier2_summary.ok()) {
        return tier2_summary.status();
    }

    absl::StatusOr<std::string> tier3_ref =
        ParseRequiredNonEmptyString(response, "tier3_ref", "mid-term compactor response");
    if (!tier3_ref.ok()) {
        return tier3_ref.status();
    }

    const json& keywords_json = response.at("tier3_keywords");
    if (!keywords_json.is_array()) {
        return invalid_argument(
            "mid-term compactor response field 'tier3_keywords' must be an array");
    }
    if (keywords_json.size() != 5U) {
        return invalid_argument(
            "mid-term compactor response field 'tier3_keywords' must contain exactly 5 items");
    }
    std::vector<std::string> tier3_keywords;
    tier3_keywords.reserve(keywords_json.size());
    for (std::size_t index = 0; index < keywords_json.size(); ++index) {
        if (!keywords_json[index].is_string()) {
            return invalid_argument(
                "mid-term compactor response field 'tier3_keywords' must contain only strings");
        }
        const std::string keyword = keywords_json[index].get<std::string>();
        if (keyword.empty()) {
            return invalid_argument(
                "mid-term compactor response field 'tier3_keywords' must not contain empty "
                "strings");
        }
        tier3_keywords.push_back(keyword);
    }

    const json& salience_json = response.at("salience");
    if (!salience_json.is_number_integer()) {
        return invalid_argument("mid-term compactor response field 'salience' must be an integer");
    }
    const int salience = salience_json.get<int>();
    if (salience < 1 || salience > 10) {
        return invalid_argument(
            "mid-term compactor response field 'salience' must be in the range 1-10");
    }

    return CompactedMidTermEpisode{
        .tier1_detail = std::move(tier1_detail),
        .tier2_summary = std::move(*tier2_summary),
        .tier3_ref = std::move(*tier3_ref),
        .tier3_keywords = std::move(tier3_keywords),
        .salience = salience,
        // TODO: Populate embeddings via an embedding service once the compaction pipeline
        // defines where that enrichment step should happen.
        .embedding = {},
    };
}

class LlmMidTermCompactor final : public MidTermCompactor {
  public:
    LlmMidTermCompactor(std::shared_ptr<const OpenAiResponsesClient> responses_client,
                        std::string model, std::string system_prompt)
        : responses_client_(std::move(responses_client)), model_(std::move(model)),
          system_prompt_(std::move(system_prompt)) {}

    [[nodiscard]] absl::StatusOr<CompactedMidTermEpisode>
    Compact(const MidTermCompactionRequest& request) override {
        VLOG(1) << "LlmMidTermCompactor compacting session_id="
                << SanitizeForLog(request.session_id)
                << " conversation_item_index=" << request.flush_candidate.conversation_item_index
                << " message_count=" << request.flush_candidate.ongoing_episode.messages.size();

        const json input_json =
            SerializeEpisodeForCompactor(request.flush_candidate.ongoing_episode);
        const std::string user_text = input_json.dump();

        std::string output_text;
        absl::Status stream_status = responses_client_->StreamResponse(
            OpenAiResponsesRequest{
                .model = model_,
                .system_prompt = system_prompt_,
                .user_text = user_text,
            },
            [&output_text](const OpenAiResponsesEvent& event) -> absl::Status {
                return std::visit(
                    [&output_text](const auto& concrete_event) -> absl::Status {
                        using Event = std::decay_t<decltype(concrete_event)>;
                        if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                            output_text.append(concrete_event.text_delta);
                        }
                        return absl::OkStatus();
                    },
                    event);
            });

        if (!stream_status.ok()) {
            LOG(WARNING) << "LlmMidTermCompactor LLM call failed session_id="
                         << SanitizeForLog(request.session_id) << " conversation_item_index="
                         << request.flush_candidate.conversation_item_index
                         << " model=" << SanitizeForLog(model_) << " detail='"
                         << SanitizeForLog(stream_status.message()) << "'";
            return stream_status;
        }

        if (output_text.empty()) {
            LOG(WARNING) << "LlmMidTermCompactor LLM returned empty response session_id="
                         << SanitizeForLog(request.session_id) << " conversation_item_index="
                         << request.flush_candidate.conversation_item_index
                         << " model=" << SanitizeForLog(model_);
            return invalid_argument("mid-term compactor LLM returned empty response");
        }

        absl::StatusOr<CompactedMidTermEpisode> compacted = ParseCompactorResponse(output_text);
        if (!compacted.ok()) {
            LOG(WARNING) << "LlmMidTermCompactor failed to parse response session_id="
                         << SanitizeForLog(request.session_id) << " conversation_item_index="
                         << request.flush_candidate.conversation_item_index
                         << " model=" << SanitizeForLog(model_) << " detail='"
                         << SanitizeForLog(compacted.status().message()) << "'";
            return compacted.status();
        }

        VLOG(1) << "LlmMidTermCompactor compacted session_id=" << SanitizeForLog(request.session_id)
                << " conversation_item_index=" << request.flush_candidate.conversation_item_index
                << " salience=" << compacted->salience
                << " tier2_bytes=" << compacted->tier2_summary.size()
                << " tier3_bytes=" << compacted->tier3_ref.size();
        return compacted;
    }

  private:
    std::shared_ptr<const OpenAiResponsesClient> responses_client_;
    std::string model_;
    std::string system_prompt_;
};

} // namespace

absl::StatusOr<MidTermCompactorPtr> CreateLlmMidTermCompactor(
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> responses_client,
    std::string model) {
    if (!responses_client) {
        return invalid_argument("LlmMidTermCompactor requires a non-null responses client");
    }
    if (model.empty()) {
        return invalid_argument("LlmMidTermCompactor requires a non-empty model name");
    }
    absl::StatusOr<std::string> system_prompt =
        LoadPrompt(PromptAsset::kMidTermCompactorSystemPrompt);
    if (!system_prompt.ok()) {
        return system_prompt.status();
    }
    return std::make_shared<LlmMidTermCompactor>(std::move(responses_client), std::move(model),
                                                 std::move(*system_prompt));
}

} // namespace isla::server::memory
