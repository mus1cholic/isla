#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "isla/server/memory/memory_timestamp_utils.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::evals {

struct EvalConversationMessage {
    isla::server::memory::MessageRole role = isla::server::memory::MessageRole::User;
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
};

struct EvalInput {
    std::string text;
    std::optional<isla::server::memory::Timestamp> create_time;
};

// Benchmark-first case shape for the initial app-boundary eval runner.
//
// `conversation` replays prior transcript history. `input` is the user turn evaluated by the live
// responder path. `expected_answer` is benchmark-owned judge input and is not executed by the
// serving path.
struct EvalCase {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    std::vector<EvalConversationMessage> conversation;
    EvalInput input;
    std::optional<std::string> expected_answer;
};

// Generic transcript-shaped benchmark input for adapters that normalize external datasets into one
// canonical app-boundary eval case.
struct EvalBenchmarkTimelineCase {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    std::vector<EvalConversationMessage> conversation;
    EvalInput input;
    std::optional<std::string> expected_answer;
};

struct EvalPromptArtifacts {
    std::string system_prompt;
    std::string working_memory_context;
    std::string full_prompt;
};

struct EvalMidTermEpisodeArtifact {
    std::string episode_id;
    isla::server::memory::Timestamp created_at;
    int salience = 0;
    std::string tier2_summary;
    bool expandable = false;
};

struct EvalEmittedEvent {
    std::string op;
    std::string turn_id;
    std::string payload;
};

struct EvalFailure {
    std::string code;
    std::string message;
};

enum class EvalTurnStatus {
    kSucceeded,
    kFailed,
    kCancelled,
};

enum class EvalReplayEventKind {
    kSessionStart,
    kConversationMessage,
    kEvaluationReferenceTime,
};

struct EvalReplayEventArtifact {
    EvalReplayEventKind kind = EvalReplayEventKind::kConversationMessage;
    std::optional<std::string> turn_id;
    std::optional<std::string> role;
    std::optional<isla::server::memory::Timestamp> timestamp;
    std::optional<std::string> text;
};

struct EvalArtifacts {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::string evaluated_turn_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    EvalPromptArtifacts prompt;
    std::vector<EvalReplayEventArtifact> replayed_session_history;
    std::vector<EvalMidTermEpisodeArtifact> pre_turn_mid_term_episodes;
    std::vector<EvalMidTermEpisodeArtifact> post_turn_mid_term_episodes;
    std::vector<EvalEmittedEvent> emitted_events;
    EvalTurnStatus status = EvalTurnStatus::kSucceeded;
    std::optional<std::string> final_reply;
    std::optional<EvalFailure> failure;
};

} // namespace isla::server::evals
