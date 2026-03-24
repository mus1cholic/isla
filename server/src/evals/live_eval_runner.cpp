#include "server/src/evals/live_eval_runner.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "client/src/ai_gateway_client_session.hpp"

namespace isla::server::evals {
namespace {

namespace protocol = isla::shared::ai_gateway;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string ResolveGatewayConnectHost(std::string_view host) {
    if (host.empty() || host == "0.0.0.0") {
        return "127.0.0.1";
    }
    if (host == "::" || host == "[::]") {
        return "::1";
    }
    return std::string(host);
}

std::string BuildEventPayload(std::string_view code, std::string_view message) {
    return absl::StrCat(code, ":", message);
}

struct TurnCompletion {
    bool completed = false;
    bool cancelled = false;
    std::optional<EvalFailure> failure;
    std::optional<std::string> reply_text;
};

class RecordingLiveEvalSession final {
  public:
    explicit RecordingLiveEvalSession(std::chrono::milliseconds timeout) : timeout_(timeout) {}

    void RecordBenchmarkUserMessage(std::string turn_id, std::string text) {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(EvalReplayEventArtifact{
            .kind = EvalReplayEventKind::kConversationMessage,
            .turn_id = std::move(turn_id),
            .role = std::string("user"),
            .timestamp = std::nullopt,
            .text = std::move(text),
        });
    }

    void OnMessage(const protocol::GatewayMessage& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (protocol::message_type(message)) {
        case protocol::MessageType::SessionStarted: {
            const auto& started = std::get<protocol::SessionStartedMessage>(message);
            session_id_ = started.session_id;
            history_.push_back(EvalReplayEventArtifact{
                .kind = EvalReplayEventKind::kSessionStart,
                .turn_id = std::nullopt,
                .role = std::nullopt,
                .timestamp = std::nullopt,
                .text = std::nullopt,
            });
            break;
        }
        case protocol::MessageType::TextOutput: {
            const auto& output = std::get<protocol::TextOutputMessage>(message);
            turns_[output.turn_id].reply_text = output.text;
            events_.push_back(EvalEmittedEvent{
                .op = "text.output",
                .turn_id = output.turn_id,
                .payload = output.text,
            });
            history_.push_back(EvalReplayEventArtifact{
                .kind = EvalReplayEventKind::kConversationMessage,
                .turn_id = output.turn_id,
                .role = std::string("assistant"),
                .timestamp = std::nullopt,
                .text = output.text,
            });
            break;
        }
        case protocol::MessageType::TurnCompleted: {
            const auto& completed = std::get<protocol::TurnCompletedMessage>(message);
            turns_[completed.turn_id].completed = true;
            events_.push_back(EvalEmittedEvent{
                .op = "turn.completed",
                .turn_id = completed.turn_id,
                .payload = "",
            });
            cv_.notify_all();
            break;
        }
        case protocol::MessageType::TurnCancelled: {
            const auto& cancelled = std::get<protocol::TurnCancelledMessage>(message);
            turns_[cancelled.turn_id].cancelled = true;
            events_.push_back(EvalEmittedEvent{
                .op = "turn.cancelled",
                .turn_id = cancelled.turn_id,
                .payload = "",
            });
            cv_.notify_all();
            break;
        }
        case protocol::MessageType::Error: {
            const auto& error = std::get<protocol::ErrorMessage>(message);
            events_.push_back(EvalEmittedEvent{
                .op = "error",
                .turn_id = error.turn_id.value_or(""),
                .payload = BuildEventPayload(error.code, error.message),
            });
            if (error.turn_id.has_value()) {
                turns_[*error.turn_id].failure = EvalFailure{
                    .code = error.code,
                    .message = error.message,
                };
            } else {
                session_error_ = EvalFailure{
                    .code = error.code,
                    .message = error.message,
                };
            }
            cv_.notify_all();
            break;
        }
        case protocol::MessageType::SessionEnded:
        case protocol::MessageType::SessionStart:
        case protocol::MessageType::SessionEnd:
        case protocol::MessageType::TextInput:
        case protocol::MessageType::AudioOutput:
        case protocol::MessageType::TurnCancel:
            break;
        }
    }

    void OnTransportClosed(absl::Status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        transport_closed_status_ = std::move(status);
        cv_.notify_all();
    }

    [[nodiscard]] absl::StatusOr<TurnCompletion> WaitForTurn(std::string_view turn_id) const {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = cv_.wait_for(lock, timeout_, [&] {
            const auto it = turns_.find(std::string(turn_id));
            const bool terminal =
                it != turns_.end() &&
                (it->second.completed || it->second.cancelled || it->second.failure.has_value());
            return terminal || transport_closed_status_.has_value() || session_error_.has_value();
        });
        if (!ready) {
            return absl::DeadlineExceededError(
                absl::StrCat("timed out waiting for gateway turn completion turn_id=", turn_id));
        }
        if (transport_closed_status_.has_value() && !transport_closed_status_->ok()) {
            return *transport_closed_status_;
        }
        if (const auto it = turns_.find(std::string(turn_id)); it != turns_.end()) {
            return it->second;
        }
        if (session_error_.has_value()) {
            return absl::FailedPreconditionError(absl::StrCat(
                "gateway session failed before turn completed code=", session_error_->code,
                " message=", session_error_->message));
        }
        return absl::FailedPreconditionError(
            absl::StrCat("gateway turn did not complete turn_id=", turn_id));
    }

    [[nodiscard]] std::optional<std::string> session_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_id_;
    }

    [[nodiscard]] std::vector<EvalReplayEventArtifact> history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    [[nodiscard]] std::vector<EvalEmittedEvent> FilterTurnEvents(std::string_view turn_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<EvalEmittedEvent> filtered;
        for (const EvalEmittedEvent& event : events_) {
            if (event.turn_id == turn_id) {
                filtered.push_back(event);
            }
        }
        return filtered;
    }

  private:
    std::chrono::milliseconds timeout_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    absl::flat_hash_map<std::string, TurnCompletion> turns_;
    std::vector<EvalReplayEventArtifact> history_;
    std::vector<EvalEmittedEvent> events_;
    std::optional<std::string> session_id_;
    std::optional<EvalFailure> session_error_;
    std::optional<absl::Status> transport_closed_status_;
};

absl::Status ValidateLiveEvalCase(const EvalCase& eval_case) {
    if (eval_case.benchmark_name.empty()) {
        return invalid_argument("eval case must include benchmark_name");
    }
    if (eval_case.case_id.empty()) {
        return invalid_argument("eval case must include case_id");
    }
    if (eval_case.session_id.empty()) {
        return invalid_argument("eval case must include session_id");
    }
    if (eval_case.input.text.empty()) {
        return invalid_argument("live eval case input.text must not be empty");
    }
    for (const EvalConversationMessage& message : eval_case.conversation) {
        if (message.role == isla::server::memory::MessageRole::User && message.text.empty()) {
            return invalid_argument("live eval case conversation user text must not be empty");
        }
    }
    return absl::OkStatus();
}

EvalTurnStatus ToEvalTurnStatus(const TurnCompletion& completion) {
    if (completion.cancelled) {
        return EvalTurnStatus::kCancelled;
    }
    if (completion.failure.has_value()) {
        return EvalTurnStatus::kFailed;
    }
    return EvalTurnStatus::kSucceeded;
}

} // namespace

LiveEvalRunner::LiveEvalRunner(LiveEvalRunnerConfig config) : config_(std::move(config)) {}

absl::StatusOr<EvalArtifacts> LiveEvalRunner::RunCase(const EvalCase& eval_case) const {
    if (absl::Status status = ValidateLiveEvalCase(eval_case); !status.ok()) {
        return status;
    }
    if (config_.port == 0U) {
        return invalid_argument("live eval runner requires a non-zero gateway port");
    }

    auto recorder = std::make_shared<RecordingLiveEvalSession>(config_.operation_timeout);
    isla::client::AiGatewayClientSession session(isla::client::AiGatewayClientConfig{
        .host = ResolveGatewayConnectHost(config_.host),
        .port = config_.port,
        .path = config_.path,
        .operation_timeout = config_.operation_timeout,
        .on_message =
            [recorder](const protocol::GatewayMessage& message) { recorder->OnMessage(message); },
        .on_transport_closed =
            [recorder](absl::Status status) { recorder->OnTransportClosed(std::move(status)); },
    });

    const absl::Status connect_status = session.ConnectAndStart(eval_case.session_id);
    if (!connect_status.ok()) {
        return connect_status;
    }

    std::size_t next_history_turn = 1U;
    for (const EvalConversationMessage& message : eval_case.conversation) {
        if (message.role != isla::server::memory::MessageRole::User) {
            continue;
        }
        const std::string turn_id = absl::StrCat("history_turn_", next_history_turn++);
        recorder->RecordBenchmarkUserMessage(turn_id, message.text);
        if (const absl::Status send_status = session.SendTextInput(turn_id, message.text);
            !send_status.ok()) {
            session.Close();
            return send_status;
        }
        const absl::StatusOr<TurnCompletion> completion = recorder->WaitForTurn(turn_id);
        if (!completion.ok()) {
            session.Close();
            return completion.status();
        }
        if (completion->cancelled || completion->failure.has_value()) {
            session.Close();
            return absl::FailedPreconditionError(
                absl::StrCat("gateway history replay turn failed turn_id=", turn_id));
        }
    }

    constexpr std::string_view kEvaluatedTurnId = "evaluated_turn";
    recorder->RecordBenchmarkUserMessage(std::string(kEvaluatedTurnId), eval_case.input.text);
    if (const absl::Status send_status =
            session.SendTextInput(std::string(kEvaluatedTurnId), eval_case.input.text);
        !send_status.ok()) {
        session.Close();
        return send_status;
    }
    const absl::StatusOr<TurnCompletion> completion = recorder->WaitForTurn(kEvaluatedTurnId);

    if (session.is_open()) {
        static_cast<void>(session.EndSession());
    }
    session.Close();

    if (!completion.ok()) {
        return completion.status();
    }

    EvalArtifacts artifacts{
        .benchmark_name = eval_case.benchmark_name,
        .case_id = eval_case.case_id,
        .session_id = recorder->session_id().value_or(eval_case.session_id),
        .evaluated_turn_id = std::string(kEvaluatedTurnId),
        .session_start_time = std::nullopt,
        .evaluation_reference_time = std::nullopt,
        .prompt = EvalPromptArtifacts{},
        .replayed_session_history = recorder->history(),
        .pre_turn_mid_term_episodes = {},
        .post_turn_mid_term_episodes = {},
        .emitted_events = recorder->FilterTurnEvents(kEvaluatedTurnId),
        .status = ToEvalTurnStatus(*completion),
        .final_reply = completion->reply_text,
        .failure = completion->failure,
    };
    return artifacts;
}

} // namespace isla::server::evals
