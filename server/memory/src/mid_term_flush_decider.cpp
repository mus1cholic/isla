#include "isla/server/memory/mid_term_flush_decider.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/llm_client.hpp"
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

// ---------------------------------------------------------------------------
// ID helpers: "i3" <-> index 3, "m7" <-> index 7
// ---------------------------------------------------------------------------

std::string MakeItemId(std::size_t index) {
    return "i" + std::to_string(index);
}

std::string MakeMessageId(std::size_t index) {
    return "m" + std::to_string(index);
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
        // Verify no trailing garbage (stoull stops at first non-digit).
        if (std::to_string(value) != numeric_part) {
            return invalid_argument("id '" + id + "' contains non-numeric characters");
        }
        return value;
    } catch (...) {
        return invalid_argument("id '" + id + "' has an invalid numeric suffix");
    }
}

// ---------------------------------------------------------------------------
// Conversation -> JSON serialization
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// JSON response -> MidTermFlushDecision parsing
// ---------------------------------------------------------------------------

absl::StatusOr<MidTermFlushDecision> ParseDeciderResponse(const std::string& response_text) {
    json response;
    try {
        response = json::parse(response_text);
    } catch (const json::parse_error& error) {
        // Fallback: some models wrap JSON in markdown code fences despite prompt instructions.
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

    if (!response.contains("should_flush") || !response["should_flush"].is_boolean()) {
        return invalid_argument("flush decider response missing boolean 'should_flush' field");
    }
    const bool should_flush = response["should_flush"].get<bool>();

    if (!should_flush) {
        // Validate consistency: flush-only fields should be null when not flushing.
        if (response.contains("item_id") && !response["item_id"].is_null()) {
            return invalid_argument(
                "flush decider returned should_flush=false but item_id is non-null");
        }
        if (response.contains("split_at") && !response["split_at"].is_null()) {
            return invalid_argument(
                "flush decider returned should_flush=false but split_at is non-null");
        }
        return MidTermFlushDecision{ .should_flush = false };
    }

    // should_flush == true: item_id is required.
    if (!response.contains("item_id") || response["item_id"].is_null()) {
        return invalid_argument("flush decider returned should_flush=true but item_id is null");
    }
    if (!response["item_id"].is_string()) {
        return invalid_argument("flush decider response item_id must be a string");
    }
    const std::string item_id = response["item_id"].get<std::string>();
    const absl::StatusOr<std::size_t> conversation_item_index = ParseIdWithPrefix(item_id, 'i');
    if (!conversation_item_index.ok()) {
        return invalid_argument("flush decider returned invalid item_id '" + item_id +
                                "': " + std::string(conversation_item_index.status().message()));
    }

    // split_at is optional.
    std::optional<std::size_t> split_at_message_index;
    if (response.contains("split_at") && !response["split_at"].is_null()) {
        if (!response["split_at"].is_string()) {
            return invalid_argument("flush decider response split_at must be a string or null");
        }
        const std::string split_at = response["split_at"].get<std::string>();
        const absl::StatusOr<std::size_t> parsed_split = ParseIdWithPrefix(split_at, 'm');
        if (!parsed_split.ok()) {
            return invalid_argument("flush decider returned invalid split_at '" + split_at +
                                    "': " + std::string(parsed_split.status().message()));
        }
        split_at_message_index = *parsed_split;
    }

    return MidTermFlushDecision{
        .should_flush = true,
        .conversation_item_index = *conversation_item_index,
        .split_at_message_index = split_at_message_index,
    };
}

// ---------------------------------------------------------------------------
// Concrete implementation
// ---------------------------------------------------------------------------

class LlmMidTermFlushDecider final : public MidTermFlushDecider {
  public:
    LlmMidTermFlushDecider(std::shared_ptr<const LlmClient> llm_client, std::string model,
                           std::string system_prompt, LlmReasoningEffort reasoning_effort)
        : llm_client_(std::move(llm_client)), model_(std::move(model)),
          system_prompt_(std::move(system_prompt)), reasoning_effort_(reasoning_effort) {}

    [[nodiscard]] absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) override {
        VLOG(1) << "LlmMidTermFlushDecider deciding item_count=" << conversation.items.size();

        const json input_json = SerializeConversationForDecider(conversation);
        const std::string user_text = input_json.dump();

        // Stream the LLM response and accumulate text.
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

        // Validate parsed indices against the actual conversation.
        if (decision->should_flush && decision->conversation_item_index.has_value()) {
            const std::size_t item_idx = *decision->conversation_item_index;
            if (item_idx >= conversation.items.size()) {
                return invalid_argument("flush decider returned item_id i" +
                                        std::to_string(item_idx) + " but conversation only has " +
                                        std::to_string(conversation.items.size()) + " items");
            }
            if (decision->split_at_message_index.has_value()) {
                const ConversationItem& item = conversation.items[item_idx];
                if (item.type != ConversationItemType::OngoingEpisode ||
                    !item.ongoing_episode.has_value()) {
                    return invalid_argument("flush decider returned split_at for item i" +
                                            std::to_string(item_idx) +
                                            " which is not an ongoing episode");
                }
                const std::size_t msg_idx = *decision->split_at_message_index;
                if (msg_idx >= item.ongoing_episode->messages.size()) {
                    return invalid_argument(
                        "flush decider returned split_at m" + std::to_string(msg_idx) +
                        " but ongoing episode only has " +
                        std::to_string(item.ongoing_episode->messages.size()) + " messages");
                }
            }
        }

        VLOG(1) << "LlmMidTermFlushDecider decided should_flush="
                << (decision->should_flush ? "true" : "false") << " conversation_item_index="
                << (decision->conversation_item_index.has_value()
                        ? std::to_string(*decision->conversation_item_index)
                        : "none")
                << " split_at_message_index="
                << (decision->split_at_message_index.has_value()
                        ? std::to_string(*decision->split_at_message_index)
                        : "none");
        return decision;
    }

  private:
    std::shared_ptr<const LlmClient> llm_client_;
    std::string model_;
    std::string system_prompt_;
    LlmReasoningEffort reasoning_effort_;
};

} // namespace

// ---------------------------------------------------------------------------
// Factory function
// ---------------------------------------------------------------------------

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
