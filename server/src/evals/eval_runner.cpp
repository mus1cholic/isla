#include "isla/server/evals/eval_runner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::evals {
namespace {

using isla::server::ai_gateway::GatewayEmitCallback;
using isla::server::ai_gateway::GatewayLiveSession;
using isla::server::ai_gateway::GatewaySessionRegistry;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::SessionStartedEvent;
using isla::server::ai_gateway::TurnAcceptedEvent;
using isla::server::memory::IsExpandableEpisode;
using isla::server::memory::MessageRole;
using isla::server::memory::WorkingMemoryState;
using namespace std::chrono_literals;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(message);
}

struct RecordingSessionEvent {
    std::string op;
    std::string turn_id;
    std::string payload;
};

class RecordingLiveSession final : public GatewayLiveSession {
  public:
    explicit RecordingLiveSession(std::string session_id) : session_id_(std::move(session_id)) {}

    [[nodiscard]] const std::string& session_id() const override {
        return session_id_;
    }

    [[nodiscard]] bool is_closed() const override {
        return false;
    }

    void AsyncEmitTextOutput(std::string turn_id, std::string text,
                             GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "text.output",
            .turn_id = std::move(turn_id),
            .payload = std::move(text),
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitAudioOutput(std::string turn_id, std::string mime_type, std::string audio_base64,
                              GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "audio.output",
            .turn_id = std::move(turn_id),
            .payload = std::move(mime_type) + ":" + std::move(audio_base64),
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitTurnCompleted(std::string turn_id, GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "turn.completed",
            .turn_id = std::move(turn_id),
            .payload = "",
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitTurnCancelled(std::string turn_id, GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "turn.cancelled",
            .turn_id = std::move(turn_id),
            .payload = "",
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitError(std::optional<std::string> turn_id, std::string code, std::string message,
                        GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "error",
            .turn_id = turn_id.value_or(""),
            .payload = std::move(code) + ":" + std::move(message),
        });
        on_complete(absl::OkStatus());
    }

    [[nodiscard]] bool WaitForTurnTerminal(std::string_view turn_id,
                                           std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout,
                            [this, turn_id] { return HasTerminalEventLocked(turn_id); });
    }

    [[nodiscard]] std::vector<RecordingSessionEvent> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

  private:
    [[nodiscard]] bool HasTerminalEventLocked(std::string_view turn_id) const {
        return std::any_of(
            events_.begin(), events_.end(), [turn_id](const RecordingSessionEvent& event) {
                return event.turn_id == turn_id &&
                       (event.op == "turn.completed" || event.op == "turn.cancelled");
            });
    }

    void RecordEvent(RecordingSessionEvent event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        cv_.notify_all();
    }

    std::string session_id_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<RecordingSessionEvent> events_;
};

class ResponderRegistryAttachment final {
  public:
    explicit ResponderRegistryAttachment(GatewayStubResponder& responder)
        : responder_(responder), registry_(&responder_) {
        responder_.AttachSessionRegistry(&registry_);
    }

    ~ResponderRegistryAttachment() {
        registry_.NotifyServerStopping();
        responder_.AttachSessionRegistry(nullptr);
    }

    [[nodiscard]] GatewaySessionRegistry& registry() {
        return registry_;
    }

  private:
    GatewayStubResponder& responder_;
    GatewaySessionRegistry registry_;
};

struct ReplayMessage {
    std::string turn_id;
    MessageRole role = MessageRole::User;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
};

struct ReplayPlan {
    std::vector<ReplayMessage> history_messages;
    std::string evaluated_turn_id;
};

class ReplaySessionClock final : public isla::server::ai_gateway::GatewaySessionClock {
  public:
    ReplaySessionClock(std::string session_id,
                       std::optional<isla::server::memory::Timestamp> session_start_time,
                       std::optional<isla::server::memory::Timestamp> evaluation_reference_time,
                       absl::flat_hash_map<std::string,
                                           std::optional<isla::server::memory::Timestamp>>
                           replay_times)
        : session_id_(std::move(session_id)), session_start_time_(session_start_time),
          evaluation_reference_time_(evaluation_reference_time),
          replay_times_(std::move(replay_times)) {}

    [[nodiscard]] std::optional<isla::server::memory::Timestamp>
    ResolveSessionStartTime(std::string_view session_id) const override {
        if (session_id != session_id_) {
            return std::nullopt;
        }
        return session_start_time_;
    }

    [[nodiscard]] std::optional<isla::server::memory::Timestamp>
    ResolveConversationMessageTime(std::string_view session_id, std::string_view turn_id,
                                   isla::server::memory::MessageRole role) const override {
        if (session_id != session_id_) {
            return std::nullopt;
        }
        const auto it = replay_times_.find(
            absl::StrCat(turn_id, role == MessageRole::User ? "|user" : "|assistant"));
        if (it == replay_times_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::optional<isla::server::memory::Timestamp>
    ResolveEvaluationReferenceTime(std::string_view session_id) const override {
        if (session_id != session_id_) {
            return std::nullopt;
        }
        return evaluation_reference_time_;
    }

  private:
    std::string session_id_;
    std::optional<isla::server::memory::Timestamp> session_start_time_;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time_;
    absl::flat_hash_map<std::string, std::optional<isla::server::memory::Timestamp>> replay_times_;
};

std::optional<EvalFailure> ExtractFailure(const std::vector<RecordingSessionEvent>& events,
                                          std::string_view turn_id) {
    const auto it =
        std::find_if(events.begin(), events.end(), [turn_id](const RecordingSessionEvent& event) {
            return event.turn_id == turn_id && event.op == "error";
        });
    if (it == events.end()) {
        return std::nullopt;
    }
    const std::size_t delimiter = it->payload.find(':');
    if (delimiter == std::string::npos) {
        return EvalFailure{
            .code = "unknown_error",
            .message = it->payload,
        };
    }
    return EvalFailure{
        .code = it->payload.substr(0, delimiter),
        .message = it->payload.substr(delimiter + 1U),
    };
}

std::optional<std::string> EmptyStringAsNull(std::optional<std::string> value) {
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> ExtractFinalReply(const std::vector<RecordingSessionEvent>& events,
                                             std::string_view turn_id) {
    const auto it =
        std::find_if(events.rbegin(), events.rend(), [turn_id](const RecordingSessionEvent& event) {
            return event.turn_id == turn_id && event.op == "text.output";
        });
    if (it == events.rend()) {
        return std::nullopt;
    }
    return EmptyStringAsNull(it->payload);
}

std::vector<EvalEmittedEvent> FilterTurnEvents(const std::vector<RecordingSessionEvent>& events,
                                               std::string_view turn_id) {
    std::vector<EvalEmittedEvent> filtered;
    for (const RecordingSessionEvent& event : events) {
        if (event.turn_id != turn_id) {
            continue;
        }
        filtered.push_back(EvalEmittedEvent{
            .op = event.op,
            .turn_id = event.turn_id,
            .payload = event.payload,
        });
    }
    return filtered;
}

std::vector<EvalMidTermEpisodeArtifact> BuildMidTermArtifacts(const WorkingMemoryState& state) {
    std::vector<EvalMidTermEpisodeArtifact> artifacts;
    artifacts.reserve(state.mid_term_episodes.size());
    for (const isla::server::memory::Episode& episode : state.mid_term_episodes) {
        artifacts.push_back(EvalMidTermEpisodeArtifact{
            .episode_id = episode.episode_id,
            .created_at = episode.created_at,
            .salience = episode.salience,
            .tier2_summary = episode.tier2_summary,
            .expandable = IsExpandableEpisode(episode),
        });
    }
    return artifacts;
}

std::string BuildEvalMemoryUserId(std::string_view benchmark_name) {
    std::string normalized = "eval";
    normalized.reserve(5U + benchmark_name.size());
    normalized.push_back('_');

    bool previous_was_separator = true;
    for (const unsigned char ch : benchmark_name) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
            previous_was_separator = false;
            continue;
        }
        if (!previous_was_separator) {
            normalized.push_back('_');
            previous_was_separator = true;
        }
    }
    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    if (normalized == "eval") {
        return "eval_session";
    }
    return normalized;
}

std::vector<EvalReplayEventArtifact>
BuildReplayedSessionHistoryArtifacts(const EvalCase& eval_case, std::string_view evaluated_turn_id,
                                     const std::optional<std::string>& final_reply) {
    std::vector<EvalReplayEventArtifact> history;
    history.reserve(eval_case.conversation.size() + 3U +
                    (eval_case.session_start_time.has_value() ? 1U : 0U) +
                    (eval_case.evaluation_reference_time.has_value() ? 1U : 0U));

    if (eval_case.session_start_time.has_value()) {
        history.push_back(EvalReplayEventArtifact{
            .kind = EvalReplayEventKind::kSessionStart,
            .turn_id = std::nullopt,
            .role = std::nullopt,
            .timestamp = eval_case.session_start_time,
            .text = std::nullopt,
        });
    }

    auto append_message_event =
        [&history](MessageRole role, std::string text,
                   std::optional<isla::server::memory::Timestamp> timestamp,
                   std::optional<std::string> turn_id) {
            history.push_back(EvalReplayEventArtifact{
                .kind = EvalReplayEventKind::kConversationMessage,
                .turn_id = std::move(turn_id),
                .role = std::string(role == MessageRole::User ? "user" : "assistant"),
                .timestamp = timestamp,
                .text = std::move(text),
            });
        };

    for (const EvalConversationMessage& message : eval_case.conversation) {
        append_message_event(message.role, message.text, message.create_time, std::nullopt);
    }
    append_message_event(MessageRole::User, eval_case.input.text, eval_case.input.create_time,
                         std::string(evaluated_turn_id));
    if (final_reply.has_value()) {
        append_message_event(MessageRole::Assistant, *final_reply, std::nullopt,
                             std::string(evaluated_turn_id));
    }

    if (eval_case.evaluation_reference_time.has_value()) {
        history.push_back(EvalReplayEventArtifact{
            .kind = EvalReplayEventKind::kEvaluationReferenceTime,
            .turn_id = std::nullopt,
            .role = std::nullopt,
            .timestamp = eval_case.evaluation_reference_time,
            .text = std::nullopt,
        });
    }

    return history;
}

absl::Status ValidateConversationMessage(const EvalConversationMessage& message,
                                         std::size_t index) {
    if (message.text.empty()) {
        return invalid_argument(
            absl::StrCat("conversation message at index ", index, " must include non-empty text"));
    }
    return absl::OkStatus();
}

absl::Status ValidateInput(const EvalInput& input) {
    if (input.text.empty()) {
        return invalid_argument("eval case input must include non-empty text");
    }
    return absl::OkStatus();
}

std::string ReplayTimestampKey(std::string_view turn_id, MessageRole role) {
    return absl::StrCat(turn_id, role == MessageRole::User ? "|user" : "|assistant");
}

absl::StatusOr<ReplayPlan> BuildReplayPlan(const EvalCase& eval_case) {
    ReplayPlan plan{
        .evaluated_turn_id = "evaluated_turn",
    };

    std::size_t next_history_turn = 1U;
    std::optional<std::string> open_turn_id = std::nullopt;
    for (std::size_t index = 0; index < eval_case.conversation.size(); ++index) {
        const EvalConversationMessage& message = eval_case.conversation[index];
        if (message.role == MessageRole::User) {
            open_turn_id = absl::StrCat("history_turn_", next_history_turn++);
            plan.history_messages.push_back(ReplayMessage{
                .turn_id = *open_turn_id,
                .role = message.role,
                .text = message.text,
                .create_time = message.create_time,
            });
            continue;
        }
        if (!open_turn_id.has_value()) {
            return invalid_argument(absl::StrCat("conversation message at index ", index,
                                                 " (role=assistant) must follow a prior user "
                                                 "message"));
        }
        plan.history_messages.push_back(ReplayMessage{
            .turn_id = *open_turn_id,
            .role = message.role,
            .text = message.text,
            .create_time = message.create_time,
        });
        open_turn_id.reset();
    }
    return plan;
}

absl::Status ValidateCase(const EvalCase& eval_case) {
    if (eval_case.benchmark_name.empty()) {
        return invalid_argument("eval case must include benchmark_name");
    }
    if (eval_case.case_id.empty()) {
        return invalid_argument("eval case must include case_id");
    }
    if (eval_case.session_id.empty()) {
        return invalid_argument("eval case must include session_id");
    }
    for (std::size_t index = 0; index < eval_case.conversation.size(); ++index) {
        if (absl::Status status = ValidateConversationMessage(eval_case.conversation[index], index);
            !status.ok()) {
            return status;
        }
    }
    if (absl::Status status = ValidateInput(eval_case.input); !status.ok()) {
        return status;
    }
    if (absl::StatusOr<ReplayPlan> plan = BuildReplayPlan(eval_case); !plan.ok()) {
        return invalid_argument(
            absl::StrCat("eval case replay validation failed: ", plan.status().message()));
    }
    return absl::OkStatus();
}

absl::Status ValidateBenchmarkTimelineCase(const EvalBenchmarkTimelineCase& timeline_case) {
    return ValidateCase(EvalCase{
        .benchmark_name = timeline_case.benchmark_name,
        .case_id = timeline_case.case_id,
        .session_id = timeline_case.session_id,
        .session_start_time = timeline_case.session_start_time,
        .evaluation_reference_time = timeline_case.evaluation_reference_time,
        .conversation = timeline_case.conversation,
        .input = timeline_case.input,
        .expected_answer = timeline_case.expected_answer,
    });
}

} // namespace

EvalRunner::EvalRunner(EvalRunnerConfig config) : config_(std::move(config)) {}

absl::StatusOr<EvalCase>
BuildEvalCaseFromBenchmarkTimeline(const EvalBenchmarkTimelineCase& timeline_case) {
    if (absl::Status status = ValidateBenchmarkTimelineCase(timeline_case); !status.ok()) {
        return status;
    }

    return EvalCase{
        .benchmark_name = timeline_case.benchmark_name,
        .case_id = timeline_case.case_id,
        .session_id = timeline_case.session_id,
        .session_start_time = timeline_case.session_start_time,
        .evaluation_reference_time = timeline_case.evaluation_reference_time,
        .conversation = timeline_case.conversation,
        .input = timeline_case.input,
        .expected_answer = timeline_case.expected_answer,
    };
}

absl::StatusOr<EvalArtifacts> EvalRunner::RunCase(const EvalCase& eval_case) const {
    if (absl::Status status = ValidateCase(eval_case); !status.ok()) {
        return status;
    }
    const absl::StatusOr<ReplayPlan> replay_plan = BuildReplayPlan(eval_case);
    if (!replay_plan.ok()) {
        return replay_plan.status();
    }
    absl::flat_hash_map<std::string, std::optional<isla::server::memory::Timestamp>> replay_times;
    for (const ReplayMessage& message : replay_plan->history_messages) {
        replay_times.insert_or_assign(ReplayTimestampKey(message.turn_id, message.role),
                                      message.create_time);
    }
    replay_times.insert_or_assign(
        ReplayTimestampKey(replay_plan->evaluated_turn_id, MessageRole::User),
        eval_case.input.create_time);

    std::optional<EvalPromptArtifacts> captured_prompt;
    bool capture_next_prompt = false;

    isla::server::ai_gateway::GatewayStubResponderConfig responder_config =
        config_.responder_config;
    responder_config.memory_user_id = BuildEvalMemoryUserId(eval_case.benchmark_name);
    responder_config.session_clock = std::make_shared<const ReplaySessionClock>(
        eval_case.session_id, eval_case.session_start_time, eval_case.evaluation_reference_time,
        replay_times);
    const auto user_query_hook = responder_config.on_user_query_memory_ready;
    responder_config.on_user_query_memory_ready =
        [&captured_prompt, &capture_next_prompt, &eval_case,
         user_query_hook](std::string_view session_id,
                          const isla::server::memory::UserQueryMemoryResult& result) {
            if (capture_next_prompt && session_id == eval_case.session_id) {
                captured_prompt = EvalPromptArtifacts{
                    .system_prompt = result.rendered_system_prompt,
                    .working_memory_context = result.rendered_working_memory_context,
                    .full_prompt = result.rendered_working_memory,
                };
                capture_next_prompt = false;
            }
            if (user_query_hook) {
                user_query_hook(session_id, result);
            }
        };

    GatewayStubResponder responder(std::move(responder_config));
    ResponderRegistryAttachment registry_scope(responder);
    auto session = std::make_shared<RecordingLiveSession>(eval_case.session_id);
    registry_scope.registry().RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = eval_case.session_id });

    for (const ReplayMessage& message : replay_plan->history_messages) {
        if (message.role == MessageRole::User) {
            if (absl::Status status = responder.AppendSessionUserMessage(
                    eval_case.session_id, message.turn_id, message.text);
                !status.ok()) {
                return status;
            }
            continue;
        }
        if (absl::Status status = responder.AppendSessionAssistantMessage(
                eval_case.session_id, message.turn_id, message.text);
            !status.ok()) {
            return status;
        }
    }

    const absl::StatusOr<WorkingMemoryState> pre_turn_state =
        responder.SnapshotSessionWorkingMemoryState(eval_case.session_id, config_.event_timeout);
    if (!pre_turn_state.ok()) {
        return pre_turn_state.status();
    }

    capture_next_prompt = true;
    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = eval_case.session_id,
        .turn_id = replay_plan->evaluated_turn_id,
        .text = eval_case.input.text,
        .telemetry_context = isla::server::ai_gateway::MakeTurnTelemetryContext(
            eval_case.session_id, replay_plan->evaluated_turn_id, config_.telemetry_sink),
    });
    if (!session->WaitForTurnTerminal(replay_plan->evaluated_turn_id, config_.event_timeout)) {
        return absl::DeadlineExceededError("timed out waiting for evaluated turn to terminate");
    }
    if (!captured_prompt.has_value()) {
        return absl::FailedPreconditionError(
            "eval runner did not capture rendered prompt artifacts for evaluated turn");
    }

    const absl::StatusOr<WorkingMemoryState> post_turn_state =
        responder.SnapshotSessionWorkingMemoryState(eval_case.session_id, config_.event_timeout);
    if (!post_turn_state.ok()) {
        return post_turn_state.status();
    }

    const std::vector<RecordingSessionEvent> events = session->events();
    EvalArtifacts artifacts{
        .benchmark_name = eval_case.benchmark_name,
        .case_id = eval_case.case_id,
        .session_id = eval_case.session_id,
        .evaluated_turn_id = replay_plan->evaluated_turn_id,
        .session_start_time = eval_case.session_start_time,
        .evaluation_reference_time = eval_case.evaluation_reference_time,
        .prompt = *captured_prompt,
        .replayed_session_history = BuildReplayedSessionHistoryArtifacts(
            eval_case, replay_plan->evaluated_turn_id,
            ExtractFinalReply(events, replay_plan->evaluated_turn_id)),
        .pre_turn_mid_term_episodes = BuildMidTermArtifacts(*pre_turn_state),
        .post_turn_mid_term_episodes = BuildMidTermArtifacts(*post_turn_state),
        .emitted_events = FilterTurnEvents(events, replay_plan->evaluated_turn_id),
    };

    if (std::any_of(artifacts.emitted_events.begin(), artifacts.emitted_events.end(),
                    [](const EvalEmittedEvent& event) { return event.op == "turn.cancelled"; })) {
        artifacts.status = EvalTurnStatus::kCancelled;
    } else if (const std::optional<EvalFailure> failure =
                   ExtractFailure(events, replay_plan->evaluated_turn_id);
               failure.has_value()) {
        artifacts.status = EvalTurnStatus::kFailed;
        artifacts.failure = failure;
    } else {
        artifacts.status = EvalTurnStatus::kSucceeded;
    }

    artifacts.final_reply = ExtractFinalReply(events, replay_plan->evaluated_turn_id);
    return artifacts;
}

} // namespace isla::server::evals
