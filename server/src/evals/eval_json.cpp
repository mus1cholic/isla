#include "isla/server/evals/eval_json.hpp"

#include <fstream>
#include <string_view>

#include "absl/status/status.h"
#include <nlohmann/json.hpp>

namespace isla::server::evals {
namespace {

using nlohmann::json;
using nlohmann::ordered_json;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string TurnStatusToString(EvalTurnStatus status) {
    switch (status) {
    case EvalTurnStatus::kSucceeded:
        return "succeeded";
    case EvalTurnStatus::kFailed:
        return "failed";
    case EvalTurnStatus::kCancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string TimelineEventKindToString(EvalTimelineEventKind kind) {
    switch (kind) {
    case EvalTimelineEventKind::kSessionStart:
        return "session_start";
    case EvalTimelineEventKind::kConversationMessage:
        return "conversation_message";
    case EvalTimelineEventKind::kEvaluationReferenceTime:
        return "evaluation_reference_time";
    }
    return "unknown";
}

std::string MessageRoleToString(isla::server::memory::MessageRole role) {
    switch (role) {
    case isla::server::memory::MessageRole::User:
        return "user";
    case isla::server::memory::MessageRole::Assistant:
        return "assistant";
    }
    return "unknown";
}

json TimelineEventToJson(const EvalTimelineEventArtifact& event) {
    json payload{
        { "turn_id", event.turn_id },
        { "role", event.role },
        { "timestamp", event.timestamp },
        { "text", event.text },
    };
    if (event.kind != EvalTimelineEventKind::kConversationMessage) {
        payload["kind"] = TimelineEventKindToString(event.kind);
    }
    return payload;
}

json MidTermEpisodeToJson(const EvalMidTermEpisodeArtifact& episode) {
    return json{
        { "episode_id", episode.episode_id }, { "created_at", episode.created_at },
        { "salience", episode.salience },     { "tier2_summary", episode.tier2_summary },
        { "expandable", episode.expandable },
    };
}

} // namespace

json EvalConversationMessageToJson(const EvalConversationMessage& message) {
    return json{
        { "role", MessageRoleToString(message.role) },
        { "text", message.text },
        { "timestamp", message.create_time },
    };
}

json EvalInputToJson(const EvalInput& input) {
    return json{
        { "role", "user" },
        { "text", input.text },
        { "timestamp", input.create_time },
    };
}

json EvalCaseToJson(const EvalCase& eval_case) {
    json conversation = json::array();
    for (const EvalConversationMessage& message : eval_case.conversation) {
        conversation.push_back(EvalConversationMessageToJson(message));
    }

    return json{
        { "benchmark_name", eval_case.benchmark_name },
        { "case_id", eval_case.case_id },
        { "session_id", eval_case.session_id },
        { "session_start_time", eval_case.session_start_time },
        { "evaluation_reference_time", eval_case.evaluation_reference_time },
        { "conversation", std::move(conversation) },
        { "input", EvalInputToJson(eval_case.input) },
        { "expected_answer", eval_case.expected_answer },
    };
}

json EvalArtifactsToJson(const EvalArtifacts& artifacts) {
    json benchmark_timeline = json::array();
    for (const EvalTimelineEventArtifact& event : artifacts.benchmark_timeline) {
        benchmark_timeline.push_back(TimelineEventToJson(event));
    }

    json payload{
        { "benchmark_timeline", std::move(benchmark_timeline) },
    };

    return payload;
}

template <typename JsonType>
absl::Status WriteJsonFileImpl(const std::filesystem::path& path, const JsonType& payload) {
    if (path.empty()) {
        return invalid_argument("json output path must not be empty");
    }

    std::error_code error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return absl::InternalError("failed to create output directory");
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return absl::InternalError("failed to open json output file");
    }
    output << payload.dump(2);
    if (!output.good()) {
        return absl::InternalError("failed to write json output file");
    }
    return absl::OkStatus();
}

absl::Status WriteJsonFile(const std::filesystem::path& path, const json& payload) {
    return WriteJsonFileImpl(path, payload);
}

absl::Status WriteJsonFile(const std::filesystem::path& path, const ordered_json& payload) {
    return WriteJsonFileImpl(path, payload);
}

} // namespace isla::server::evals
