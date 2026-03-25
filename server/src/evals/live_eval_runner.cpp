#include "server/src/evals/live_eval_runner.hpp"

#include <algorithm>
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
#include "client/src/ai_gateway_client_session.hpp"
#include "isla/server/memory/memory_timestamp_utils.hpp"

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

std::string BuildTranscriptSeedKey(std::string_view turn_id, std::string_view role) {
    return absl::StrCat(turn_id, "|", role);
}

std::optional<std::string>
FormatOptionalTimestamp(std::optional<isla::server::memory::Timestamp> timestamp) {
    if (!timestamp.has_value()) {
        return std::nullopt;
    }
    return isla::server::memory::FormatTimestamp(*timestamp);
}

struct TurnCompletion {
    bool completed = false;
    bool cancelled = false;
    std::optional<EvalFailure> failure;
    std::optional<std::string> reply_text;
};

struct SeedCompletion {
    bool seeded = false;
};

absl::Status HistoryReplayTurnFailureStatus(std::string_view turn_id,
                                            const TurnCompletion& completion) {
    std::string detail = absl::StrCat("gateway history replay turn failed turn_id=", turn_id);
    if (completion.cancelled) {
        absl::StrAppend(&detail, " cancelled=true");
    }
    if (completion.failure.has_value()) {
        absl::StrAppend(&detail, " code=", completion.failure->code,
                        " message=", completion.failure->message);
    }
    return absl::FailedPreconditionError(detail);
}

class RecordingLiveEvalSession final {
  public:
    RecordingLiveEvalSession(
        std::chrono::milliseconds timeout,
        std::optional<isla::server::memory::Timestamp> session_start_time,
        std::optional<isla::server::memory::Timestamp> evaluation_reference_time)
        : timeout_(timeout), session_start_time_(session_start_time),
          evaluation_reference_time_(evaluation_reference_time) {
        if (evaluation_reference_time_.has_value()) {
            history_.push_back(EvalReplayEventArtifact{
                .kind = EvalReplayEventKind::kEvaluationReferenceTime,
                .turn_id = std::nullopt,
                .role = std::nullopt,
                .timestamp = evaluation_reference_time_,
                .text = std::nullopt,
            });
        }
    }

    void
    RecordBenchmarkConversationMessage(std::string turn_id, std::string role, std::string text,
                                       std::optional<isla::server::memory::Timestamp> timestamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(EvalReplayEventArtifact{
            .kind = EvalReplayEventKind::kConversationMessage,
            .turn_id = std::move(turn_id),
            .role = std::move(role),
            .timestamp = timestamp,
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
                .timestamp = session_start_time_,
                .text = std::nullopt,
            });
            break;
        }
        case protocol::MessageType::TranscriptSeeded: {
            const auto& seeded = std::get<protocol::TranscriptSeededMessage>(message);
            seeded_messages_[BuildTranscriptSeedKey(seeded.turn_id, seeded.role)].seeded = true;
            cv_.notify_all();
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
        case protocol::MessageType::TranscriptSeed:
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

    [[nodiscard]] absl::Status WaitForTranscriptSeed(std::string_view turn_id,
                                                     std::string_view role) const {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::string seed_key = BuildTranscriptSeedKey(turn_id, role);
        const bool ready = cv_.wait_for(lock, timeout_, [&] {
            const auto it = seeded_messages_.find(seed_key);
            const bool seeded = it != seeded_messages_.end() && it->second.seeded;
            const auto turn_it = turns_.find(std::string(turn_id));
            const bool turn_failed = turn_it != turns_.end() && turn_it->second.failure.has_value();
            return seeded || turn_failed || transport_closed_status_.has_value() ||
                   session_error_.has_value();
        });
        if (!ready) {
            return absl::DeadlineExceededError(absl::StrCat(
                "timed out waiting for transcript seed acknowledgement turn_id=", turn_id,
                " role=", role));
        }
        if (transport_closed_status_.has_value() && !transport_closed_status_->ok()) {
            return *transport_closed_status_;
        }
        if (const auto it = seeded_messages_.find(seed_key);
            it != seeded_messages_.end() && it->second.seeded) {
            return absl::OkStatus();
        }
        if (const auto it = turns_.find(std::string(turn_id));
            it != turns_.end() && it->second.failure.has_value()) {
            return absl::FailedPreconditionError(absl::StrCat(
                "gateway transcript seed failed turn_id=", turn_id,
                " code=", it->second.failure->code, " message=", it->second.failure->message));
        }
        if (session_error_.has_value()) {
            return absl::FailedPreconditionError(
                absl::StrCat("gateway session failed before transcript seed acknowledged code=",
                             session_error_->code, " message=", session_error_->message));
        }
        return absl::FailedPreconditionError(absl::StrCat(
            "gateway transcript seed was not acknowledged turn_id=", turn_id, " role=", role));
    }

    [[nodiscard]] std::optional<std::string> session_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_id_;
    }

    [[nodiscard]] std::vector<EvalReplayEventArtifact> history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<EvalReplayEventArtifact> sorted = history_;
        std::stable_sort(
            sorted.begin(), sorted.end(),
            [](const EvalReplayEventArtifact& lhs, const EvalReplayEventArtifact& rhs) {
                if (lhs.timestamp.has_value() != rhs.timestamp.has_value()) {
                    return lhs.timestamp.has_value();
                }
                if (lhs.timestamp.has_value()) {
                    return *lhs.timestamp < *rhs.timestamp;
                }
                return false;
            });
        return sorted;
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
    absl::flat_hash_map<std::string, SeedCompletion> seeded_messages_;
    std::vector<EvalReplayEventArtifact> history_;
    std::vector<EvalEmittedEvent> events_;
    std::optional<std::string> session_id_;
    std::optional<isla::server::memory::Timestamp> session_start_time_;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time_;
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
        if (message.text.empty()) {
            return invalid_argument(message.role == isla::server::memory::MessageRole::User
                                        ? "live eval case conversation user text must not be empty"
                                        : "live eval case conversation assistant text must not be "
                                          "empty");
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

    auto recorder = std::make_shared<RecordingLiveEvalSession>(config_.turn_completion_timeout,
                                                               eval_case.session_start_time,
                                                               eval_case.evaluation_reference_time);
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

    absl::Status connect_status = session.ConnectAndStart(
        eval_case.session_id, FormatOptionalTimestamp(eval_case.session_start_time),
        FormatOptionalTimestamp(eval_case.evaluation_reference_time));
    if (!connect_status.ok()) {
        return connect_status;
    }

    std::size_t next_history_turn = 1U;
    std::optional<std::string> current_history_turn_id;
    for (const EvalConversationMessage& message : eval_case.conversation) {
        if (message.role == isla::server::memory::MessageRole::User ||
            !current_history_turn_id.has_value()) {
            current_history_turn_id = absl::StrCat("history_turn_", next_history_turn++);
        }
        const std::string turn_id = *current_history_turn_id;
        const std::string role =
            message.role == isla::server::memory::MessageRole::User ? "user" : "assistant";
        recorder->RecordBenchmarkConversationMessage(turn_id, role, message.text,
                                                     message.create_time);
        if (absl::Status send_status = session.SendTranscriptSeed(
                turn_id, role, message.text, FormatOptionalTimestamp(message.create_time));
            !send_status.ok()) {
            session.Close();
            return send_status;
        }
        if (absl::Status seed_status = recorder->WaitForTranscriptSeed(turn_id, role);
            !seed_status.ok()) {
            session.Close();
            return seed_status;
        }
    }

    constexpr std::string_view kEvaluatedTurnId = "evaluated_turn";
    recorder->RecordBenchmarkConversationMessage(std::string(kEvaluatedTurnId), "user",
                                                 eval_case.input.text, eval_case.input.create_time);
    if (absl::Status send_status =
            session.SendTextInput(std::string(kEvaluatedTurnId), eval_case.input.text,
                                  FormatOptionalTimestamp(eval_case.input.create_time));
        !send_status.ok()) {
        session.Close();
        return send_status;
    }
    const absl::StatusOr<TurnCompletion> completion = recorder->WaitForTurn(kEvaluatedTurnId);

    if (session.is_open()) {
        const absl::Status end_status = session.EndSession();
        if (!end_status.ok()) {
            LOG(WARNING) << "live eval runner failed to end gateway session session_id="
                         << recorder->session_id().value_or(eval_case.session_id) << " detail='"
                         << end_status.message() << "'";
        }
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
        .session_start_time = eval_case.session_start_time,
        .evaluation_reference_time = eval_case.evaluation_reference_time,
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
