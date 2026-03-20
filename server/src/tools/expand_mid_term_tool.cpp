#include "isla/server/tools/expand_mid_term_tool.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace isla::server::tools {
namespace {

using nlohmann::json;

ToolResult BuildErrorResult(const ToolCall& call, std::string_view message) {
    return ToolResult{
        .call_id = call.call_id,
        .tool_name = std::string(ExpandMidTermTool::kName),
        .output_text = std::string(message),
        .is_error = true,
    };
}

bool HasUnexpectedProperties(const json& object) {
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        if (key != "episode_id") {
            return true;
        }
    }
    return false;
}

} // namespace

ToolDefinition ExpandMidTermTool::Definition() const {
    return ToolDefinition{
        .name = std::string(kName),
        .description =
            "Loads the full Tier 1 detail for one expandable mid-term episode from the current "
            "session. Use this only when a mid-term summary is tagged [expandable] and exact "
            "recent details are needed before answering.",
        .input_json_schema = R"json({
  "type": "object",
  "properties": {
    "episode_id": {
      "type": "string",
      "description": "The mid-term episode_id to expand."
    }
  },
  "required": ["episode_id"],
  "additionalProperties": false
})json",
        .read_only = true,
    };
}

absl::StatusOr<ToolResult> ExpandMidTermTool::Execute(const ToolExecutionContext& context,
                                                      const ToolCall& call) const {
    static_cast<void>(context);
    if (call.name != kName) {
        return absl::InvalidArgumentError("expand_mid_term tool received a mismatched call name");
    }

    const json arguments = json::parse(call.arguments_json, nullptr, false);
    if (!arguments.is_object()) {
        return BuildErrorResult(
            call, "expand_mid_term requires a JSON object with a non-empty string field "
                  "'episode_id'.");
    }
    if (HasUnexpectedProperties(arguments)) {
        return BuildErrorResult(
            call, "expand_mid_term accepts only one argument: a non-empty string field "
                  "'episode_id'.");
    }
    const auto episode_id_it = arguments.find("episode_id");
    if (episode_id_it == arguments.end() || !episode_id_it->is_string() ||
        episode_id_it->get<std::string>().empty()) {
        return BuildErrorResult(
            call, "expand_mid_term requires a JSON object with a non-empty string field "
                  "'episode_id'.");
    }
    if (context.session_reader == nullptr) {
        return absl::FailedPreconditionError(
            "expand_mid_term requires a session-backed tool reader");
    }
    const std::string episode_id = episode_id_it->get<std::string>();
    const absl::StatusOr<std::string> detail =
        context.session_reader->ExpandMidTermEpisode(episode_id);
    if (!detail.ok()) {
        return BuildErrorResult(call, detail.status().message());
    }

    return ToolResult{
        .call_id = call.call_id,
        .tool_name = std::string(kName),
        .output_text = *detail,
        .is_error = false,
    };
}

} // namespace isla::server::tools
