#include "isla/server/memory/mid_term_flush_decider.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/log/log.h"
#include "absl/log/vlog_is_on.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/llm_json_utils.hpp"
#include "isla/server/memory/memory_types.hpp"
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

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string MakeItemId(std::size_t index) {
    return "i" + std::to_string(index);
}

std::string MakeMessageId(std::size_t index) {
    return "m" + std::to_string(index);
}

std::string SummarizeDecisionBoundariesForLog(const MidTermFlushDecision& decision) {
    std::string summary = "[";
    for (std::size_t i = 0; i < decision.boundaries.size(); ++i) {
        if (i > 0) {
            summary += ", ";
        }
        summary += MakeMessageId(decision.boundaries[i].starts_at_message_index);
    }
    summary += "]";
    return summary;
}

absl::StatusOr<std::size_t> ParseIdWithPrefix(const std::string& id, char prefix) {
    if (id.empty() || id[0] != prefix) {
        return invalid_argument("expected id to start with '" + std::string(1, prefix) +
                                "' but got '" + id + "'");
    }
    const std::string numeric_part = id.substr(1);
    if (numeric_part.empty()) {
        return invalid_argument("id '" + id + "' has no numeric suffix");
    }
    try {
        const std::size_t value = std::stoull(numeric_part);
        if (std::to_string(value) != numeric_part) {
            return invalid_argument("id '" + id + "' contains non-numeric characters");
        }
        return value;
    } catch (...) {
        return invalid_argument("id '" + id + "' has an invalid numeric suffix");
    }
}

json SerializeConversationForDecider(const Conversation& conversation) {
    json items_json = json::array();
    for (std::size_t i = 0; i < conversation.items.size(); ++i) {
        const ConversationItem& item = conversation.items[i];
        json item_json;
        item_json["id"] = MakeItemId(i);

        if (item.type == ConversationItemType::EpisodeStub && item.episode_stub.has_value()) {
            item_json["type"] = "episode_stub";
            item_json["episode_stub"] = {
                { "content", item.episode_stub->content },
                { "create_time", json(item.episode_stub->create_time) },
            };
        } else if (item.type == ConversationItemType::OngoingEpisode &&
                   item.ongoing_episode.has_value()) {
            item_json["type"] = "ongoing_episode";
            json messages_json = json::array();
            for (std::size_t m = 0; m < item.ongoing_episode->messages.size(); ++m) {
                const Message& msg = item.ongoing_episode->messages[m];
                messages_json.push_back({
                    { "id", MakeMessageId(m) },
                    { "role", msg.role == MessageRole::User ? "user" : "assistant" },
                    { "content", msg.content },
                    { "create_time", json(msg.create_time) },
                });
            }
            item_json["ongoing_episode"] = { { "messages", std::move(messages_json) } };
        }

        items_json.push_back(std::move(item_json));
    }
    return json{ { "items", std::move(items_json) } };
}

absl::StatusOr<std::optional<std::string>>
ParseOptionalReasoning(const json& object, std::string_view field_name, std::string_view context) {
    if (!object.contains(field_name) || object.at(field_name).is_null()) {
        return std::nullopt;
    }
    if (!object.at(field_name).is_string()) {
        return invalid_argument(std::string(context) + " field '" + std::string(field_name) +
                                "' must be a string or null");
    }
    return object.at(field_name).get<std::string>();
}

absl::Status ValidateBoundaryObjectKeys(const json& boundary) {
    if (!boundary.is_object()) {
        return invalid_argument("flush decider boundary entries must be JSON objects");
    }
    for (const auto& [key, value] : boundary.items()) {
        static_cast<void>(value);
        if (key != "starts_at" && key != "reasoning") {
            return invalid_argument("flush decider boundary contains unexpected field '" + key +
                                    "'");
        }
    }
    if (!boundary.contains("starts_at")) {
        return invalid_argument("flush decider boundary is missing required field 'starts_at'");
    }
    return absl::OkStatus();
}

absl::StatusOr<MidTermFlushDecision> ParseDeciderResponse(const std::string& response_text) {
    json response;
    try {
        response = json::parse(response_text);
    } catch (const json::parse_error& error) {
        const std::string stripped = StripMarkdownCodeFences(response_text);
        if (stripped == response_text) {
            return invalid_argument(std::string("flush decider returned invalid JSON: ") +
                                    error.what());
        }
        try {
            response = json::parse(stripped);
        } catch (const json::parse_error& inner_error) {
            return invalid_argument(
                std::string("flush decider returned invalid JSON (after stripping code fences): ") +
                inner_error.what());
        }
    }

    if (!response.is_object()) {
        return invalid_argument("flush decider response must be a JSON object");
    }
    for (const auto& [key, value] : response.items()) {
        static_cast<void>(value);
        if (key != "boundaries" && key != "tail_complete" && key != "reasoning") {
            return invalid_argument("flush decider response contains unexpected field '" + key +
                                    "'");
        }
    }
    if (!response.contains("boundaries") || !response.at("boundaries").is_array()) {
        return invalid_argument("flush decider response missing array 'boundaries' field");
    }
    if (!response.contains("tail_complete") || !response.at("tail_complete").is_boolean()) {
        return invalid_argument("flush decider response missing boolean 'tail_complete' field");
    }

    std::vector<MidTermFlushBoundary> boundaries;
    boundaries.reserve(response.at("boundaries").size());
    for (const json& boundary : response.at("boundaries")) {
        if (absl::Status status = ValidateBoundaryObjectKeys(boundary); !status.ok()) {
            return status;
        }
        if (!boundary.at("starts_at").is_string()) {
            return invalid_argument("flush decider boundary field 'starts_at' must be a string");
        }
        const std::string starts_at = boundary.at("starts_at").get<std::string>();
        absl::StatusOr<std::size_t> parsed_index = ParseIdWithPrefix(starts_at, 'm');
        if (!parsed_index.ok()) {
            return invalid_argument("flush decider returned invalid boundary starts_at '" +
                                    starts_at +
                                    "': " + std::string(parsed_index.status().message()));
        }
        absl::StatusOr<std::optional<std::string>> boundary_reasoning =
            ParseOptionalReasoning(boundary, "reasoning", "flush decider boundary");
        if (!boundary_reasoning.ok()) {
            return boundary_reasoning.status();
        }
        boundaries.push_back(MidTermFlushBoundary{
            .starts_at_message_index = *parsed_index,
            .reasoning = std::move(*boundary_reasoning),
        });
    }

    absl::StatusOr<std::optional<std::string>> reasoning =
        ParseOptionalReasoning(response, "reasoning", "flush decider response");
    if (!reasoning.ok()) {
        return reasoning.status();
    }

    return MidTermFlushDecision{
        .boundaries = std::move(boundaries),
        .tail_complete = response.at("tail_complete").get<bool>(),
        .reasoning = std::move(*reasoning),
    };
}

absl::StatusOr<std::optional<std::size_t>>
FindLiveOngoingEpisodeIndex(const Conversation& conversation) {
    if (conversation.items.empty()) {
        return std::nullopt;
    }
    const std::size_t tail_index = conversation.items.size() - 1U;
    const ConversationItem& tail_item = conversation.items.back();
    if (tail_item.type != ConversationItemType::OngoingEpisode ||
        !tail_item.ongoing_episode.has_value()) {
        return invalid_argument(
            "flush decider requires the final conversation item to be an ongoing "
            "episode");
    }
    return tail_index;
}

class LlmMidTermFlushDecider final : public MidTermFlushDecider {
  public:
    LlmMidTermFlushDecider(std::shared_ptr<const LlmClient> llm_client, std::string model,
                           std::string system_prompt, LlmReasoningEffort reasoning_effort)
        : llm_client_(std::move(llm_client)), model_(std::move(model)),
          system_prompt_(std::move(system_prompt)), reasoning_effort_(reasoning_effort) {}

    [[nodiscard]] absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) override {
        if (VLOG_IS_ON(1)) {
            std::string items_summary = "[";
            for (std::size_t i = 0; i < conversation.items.size(); ++i) {
                if (i > 0) {
                    items_summary += ", ";
                }
                const ConversationItem& item = conversation.items[i];
                if (item.type == ConversationItemType::EpisodeStub) {
                    items_summary += "stub";
                } else if (item.type == ConversationItemType::OngoingEpisode &&
                           item.ongoing_episode.has_value()) {
                    items_summary += "ongoing(" +
                                     std::to_string(item.ongoing_episode->messages.size()) +
                                     " msgs)";
                } else {
                    items_summary += "unknown";
                }
            }
            items_summary += "]";

            VLOG(1) << "LlmMidTermFlushDecider deciding item_count=" << conversation.items.size()
                    << " items=" << items_summary;
        }

        const json input_json = SerializeConversationForDecider(conversation);
        const std::string user_text = input_json.dump();

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
            LOG(ERROR) << "LlmMidTermFlushDecider LLM call failed detail='"
                       << SanitizeForLog(stream_status.message()) << "'";
            return stream_status;
        }

        if (output_text.empty()) {
            LOG(WARNING) << "LlmMidTermFlushDecider LLM returned empty response";
            return invalid_argument("flush decider LLM returned empty response");
        }

        absl::StatusOr<MidTermFlushDecision> decision = ParseDeciderResponse(output_text);
        if (!decision.ok()) {
            LOG(WARNING) << "LlmMidTermFlushDecider failed to parse response detail='"
                         << SanitizeForLog(decision.status().message()) << "'";
            return decision.status();
        }

        absl::StatusOr<std::optional<std::size_t>> live_item_index =
            FindLiveOngoingEpisodeIndex(conversation);
        if (!live_item_index.ok()) {
            if (decision->boundaries.empty() && !decision->tail_complete) {
                return decision;
            }
            return live_item_index.status();
        }
        if (!live_item_index->has_value()) {
            if (!decision->boundaries.empty() || decision->tail_complete) {
                return invalid_argument(
                    "flush decider returned boundaries for an empty conversation");
            }
            return decision;
        }

        const ConversationItem& live_item = conversation.items[live_item_index->value()];
        const OngoingEpisodeBoundaryPlan boundary_plan{
            .boundary_message_indices =
                [&decision]() {
                    std::vector<std::size_t> indices;
                    indices.reserve(decision->boundaries.size());
                    for (const MidTermFlushBoundary& boundary : decision->boundaries) {
                        indices.push_back(boundary.starts_at_message_index);
                    }
                    return indices;
                }(),
            .tail_complete = decision->tail_complete,
        };
        if (absl::Status status =
                ValidateOngoingEpisodeBoundaryPlan(*live_item.ongoing_episode, boundary_plan);
            !status.ok()) {
            LOG(WARNING) << "LlmMidTermFlushDecider rejected invalid boundary plan"
                         << " boundaries="
                         << SanitizeForLog(SummarizeDecisionBoundariesForLog(*decision))
                         << " tail_complete=" << (decision->tail_complete ? "true" : "false")
                         << " detail='" << SanitizeForLog(status.message()) << "'";
            return status;
        }

        if (VLOG_IS_ON(1)) {
            std::string boundaries_summary = "[";
            for (std::size_t i = 0; i < decision->boundaries.size(); ++i) {
                if (i > 0) {
                    boundaries_summary += ", ";
                }
                boundaries_summary +=
                    std::to_string(decision->boundaries[i].starts_at_message_index);
            }
            boundaries_summary += "]";
            VLOG(1) << "LlmMidTermFlushDecider decided boundary_count="
                    << decision->boundaries.size() << " boundaries=" << boundaries_summary
                    << " tail_complete=" << (decision->tail_complete ? "true" : "false")
                    << " reasoning='" << SanitizeForLog(decision->reasoning.value_or("")) << "'";
        }
        return decision;
    }

  private:
    std::shared_ptr<const LlmClient> llm_client_;
    std::string model_;
    std::string system_prompt_;
    LlmReasoningEffort reasoning_effort_;
};

} // namespace

absl::StatusOr<MidTermFlushDeciderPtr>
CreateLlmMidTermFlushDecider(std::shared_ptr<const isla::server::LlmClient> llm_client,
                             std::string model, isla::server::LlmReasoningEffort reasoning_effort) {
    if (!llm_client) {
        return invalid_argument("LlmMidTermFlushDecider requires a non-null llm client");
    }
    if (model.empty()) {
        return invalid_argument("LlmMidTermFlushDecider requires a non-empty model name");
    }
    absl::StatusOr<std::string> system_prompt =
        LoadPrompt(PromptAsset::kMidTermFlushDeciderSystemPrompt);
    if (!system_prompt.ok()) {
        return system_prompt.status();
    }
    return std::make_shared<LlmMidTermFlushDecider>(std::move(llm_client), std::move(model),
                                                    std::move(*system_prompt), reasoning_effort);
}

} // namespace isla::server::memory
