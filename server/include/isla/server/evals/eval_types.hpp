#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::evals {

struct EvalTurnInput {
    std::string turn_id;
    std::string user_text;
    std::optional<isla::server::memory::Timestamp> user_create_time;
    std::optional<isla::server::memory::Timestamp> assistant_create_time;
};

// Benchmark-first case shape for the initial app-boundary eval runner.
//
// Setup turns establish session state and memory prior to the evaluated turn. The runner captures
// artifacts around `evaluated_turn`.
struct EvalCase {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    std::vector<EvalTurnInput> setup_turns;
    EvalTurnInput evaluated_turn;
};

// Benchmark-owned timeline input shape for adapters that normalize external datasets into one
// canonical app-boundary eval case. The runner still evaluates exactly one `evaluated_turn_id`;
// earlier turns become setup state and later turns are not currently supported.
struct EvalBenchmarkTimelineCase {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    std::vector<EvalTurnInput> turns;
    std::string evaluated_turn_id;
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

enum class EvalTimelineEventKind {
    kSessionStart,
    kConversationMessage,
    kEvaluationReferenceTime,
};

struct EvalTimelineEventArtifact {
    EvalTimelineEventKind kind = EvalTimelineEventKind::kConversationMessage;
    std::optional<std::string> turn_id;
    std::optional<std::string> role;
    std::optional<isla::server::memory::Timestamp> timestamp;
    std::optional<std::string> text;
    bool prompt_visible = false;
    bool runtime_observed = false;
};

struct EvalArtifacts {
    std::string benchmark_name;
    std::string case_id;
    std::string session_id;
    std::string evaluated_turn_id;
    std::optional<isla::server::memory::Timestamp> session_start_time;
    std::optional<isla::server::memory::Timestamp> evaluation_reference_time;
    std::vector<EvalTurnInput> setup_turns;
    EvalTurnInput evaluated_turn;
    EvalPromptArtifacts prompt;
    std::vector<EvalTimelineEventArtifact> benchmark_timeline;
    std::vector<EvalMidTermEpisodeArtifact> pre_turn_mid_term_episodes;
    std::vector<EvalMidTermEpisodeArtifact> post_turn_mid_term_episodes;
    std::vector<EvalEmittedEvent> emitted_events;
    EvalTurnStatus status = EvalTurnStatus::kSucceeded;
    std::optional<std::string> final_reply;
    std::optional<EvalFailure> failure;
};

} // namespace isla::server::evals
