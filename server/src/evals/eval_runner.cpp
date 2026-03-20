#include "isla/server/evals/eval_runner.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
using isla::server::ai_gateway::TurnTelemetryContext;
using isla::server::memory::IsExpandableEpisode;
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
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (std::any_of(events_.begin(), events_.end(),
                                [turn_id](const RecordingSessionEvent& event) {
                                    return event.turn_id == turn_id &&
                                           (event.op == "turn.completed" ||
                                            event.op == "turn.cancelled");
                                })) {
                    return true;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    [[nodiscard]] std::vector<RecordingSessionEvent> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

  private:
    void RecordEvent(RecordingSessionEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }

    std::string session_id_;
    mutable std::mutex mutex_;
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

bool HasSucceeded(const std::vector<RecordingSessionEvent>& events, std::string_view turn_id) {
    return std::any_of(events.begin(), events.end(), [turn_id](const RecordingSessionEvent& event) {
        return event.turn_id == turn_id && event.op == "text.output";
    });
}

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

std::optional<std::string> ExtractFinalReply(const std::vector<RecordingSessionEvent>& events,
                                             std::string_view turn_id) {
    const auto it =
        std::find_if(events.rbegin(), events.rend(), [turn_id](const RecordingSessionEvent& event) {
            return event.turn_id == turn_id && event.op == "text.output";
        });
    if (it == events.rend()) {
        return std::nullopt;
    }
    return it->payload;
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

absl::Status ValidateTurnInput(const EvalTurnInput& turn, std::string_view role) {
    if (turn.turn_id.empty()) {
        return invalid_argument(absl::StrCat(role, " turn must include a non-empty turn_id"));
    }
    if (turn.user_text.empty()) {
        return invalid_argument(absl::StrCat(role, " turn must include non-empty user_text"));
    }
    return absl::OkStatus();
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
    for (const EvalTurnInput& turn : eval_case.setup_turns) {
        if (absl::Status status = ValidateTurnInput(turn, "setup"); !status.ok()) {
            return status;
        }
    }
    return ValidateTurnInput(eval_case.evaluated_turn, "evaluated");
}

absl::Status WaitForSuccessfulSetupTurn(const RecordingLiveSession& session,
                                        std::string_view turn_id,
                                        std::chrono::milliseconds timeout) {
    if (!session.WaitForTurnTerminal(turn_id, timeout)) {
        return absl::DeadlineExceededError("timed out waiting for setup turn to terminate");
    }
    const std::vector<RecordingSessionEvent> events = session.events();
    if (const std::optional<EvalFailure> failure = ExtractFailure(events, turn_id);
        failure.has_value()) {
        return absl::FailedPreconditionError("setup turn failed with public error code '" +
                                             failure->code + "'");
    }
    if (!HasSucceeded(events, turn_id)) {
        return absl::FailedPreconditionError(
            "setup turn terminated without successful text output");
    }
    return absl::OkStatus();
}

} // namespace

EvalRunner::EvalRunner(EvalRunnerConfig config) : config_(std::move(config)) {}

absl::StatusOr<EvalArtifacts> EvalRunner::RunCase(const EvalCase& eval_case) const {
    if (absl::Status status = ValidateCase(eval_case); !status.ok()) {
        return status;
    }

    std::optional<EvalPromptArtifacts> captured_prompt;
    bool capture_next_prompt = false;

    isla::server::ai_gateway::GatewayStubResponderConfig responder_config =
        config_.responder_config;
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

    for (const EvalTurnInput& turn : eval_case.setup_turns) {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = eval_case.session_id,
            .turn_id = turn.turn_id,
            .text = turn.user_text,
            .telemetry_context = isla::server::ai_gateway::MakeTurnTelemetryContext(
                eval_case.session_id, turn.turn_id, config_.telemetry_sink),
        });
        if (absl::Status status =
                WaitForSuccessfulSetupTurn(*session, turn.turn_id, config_.event_timeout);
            !status.ok()) {
            return status;
        }
    }

    const absl::StatusOr<WorkingMemoryState> pre_turn_state =
        responder.SnapshotSessionWorkingMemoryState(eval_case.session_id);
    if (!pre_turn_state.ok()) {
        return pre_turn_state.status();
    }

    capture_next_prompt = true;
    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = eval_case.session_id,
        .turn_id = eval_case.evaluated_turn.turn_id,
        .text = eval_case.evaluated_turn.user_text,
        .telemetry_context = isla::server::ai_gateway::MakeTurnTelemetryContext(
            eval_case.session_id, eval_case.evaluated_turn.turn_id, config_.telemetry_sink),
    });
    if (!session->WaitForTurnTerminal(eval_case.evaluated_turn.turn_id, config_.event_timeout)) {
        return absl::DeadlineExceededError("timed out waiting for evaluated turn to terminate");
    }
    if (!captured_prompt.has_value()) {
        return absl::FailedPreconditionError(
            "eval runner did not capture rendered prompt artifacts for evaluated turn");
    }

    const absl::StatusOr<WorkingMemoryState> post_turn_state =
        responder.SnapshotSessionWorkingMemoryState(eval_case.session_id);
    if (!post_turn_state.ok()) {
        return post_turn_state.status();
    }

    const std::vector<RecordingSessionEvent> events = session->events();
    EvalArtifacts artifacts{
        .benchmark_name = eval_case.benchmark_name,
        .case_id = eval_case.case_id,
        .session_id = eval_case.session_id,
        .evaluated_turn_id = eval_case.evaluated_turn.turn_id,
        .prompt = *captured_prompt,
        .pre_turn_mid_term_episodes = BuildMidTermArtifacts(*pre_turn_state),
        .post_turn_mid_term_episodes = BuildMidTermArtifacts(*post_turn_state),
        .emitted_events = FilterTurnEvents(events, eval_case.evaluated_turn.turn_id),
    };

    if (std::any_of(artifacts.emitted_events.begin(), artifacts.emitted_events.end(),
                    [](const EvalEmittedEvent& event) { return event.op == "turn.cancelled"; })) {
        artifacts.status = EvalTurnStatus::kCancelled;
    } else if (const std::optional<EvalFailure> failure =
                   ExtractFailure(events, eval_case.evaluated_turn.turn_id);
               failure.has_value()) {
        artifacts.status = EvalTurnStatus::kFailed;
        artifacts.failure = failure;
    } else {
        artifacts.status = EvalTurnStatus::kSucceeded;
    }

    artifacts.final_reply = ExtractFinalReply(events, eval_case.evaluated_turn.turn_id);
    return artifacts;
}

} // namespace isla::server::evals
