#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct OngoingEpisodeBoundaryPlan {
    std::vector<std::size_t> boundary_message_indices;
    bool tail_complete = false;
};

struct OngoingEpisodeMessageRange {
    std::size_t begin_message_index = 0;
    std::size_t end_message_index = 0;
};

// Appends a new empty ongoing episode item to the end of the conversation.
void BeginOngoingEpisode(Conversation& conversation);

// Appends a user message to the current tail episode, creating one first when needed.
void AppendUserMessage(Conversation& conversation, std::string content, Timestamp create_time);

// Appends an assistant message to the current tail episode, creating one first when needed.
void AppendAssistantMessage(Conversation& conversation, std::string content, Timestamp create_time);

// Appends an episode stub item without modifying any earlier conversation items.
void AppendEpisodeStub(Conversation& conversation, std::string content, Timestamp create_time);

// Replaces the referenced ongoing episode in place with a stub after validating that the target
// item exists and contains at least one message.
[[nodiscard]] absl::Status ReplaceOngoingEpisodeWithStub(Conversation& conversation,
                                                         std::size_t conversation_item_index,
                                                         std::string stub_text,
                                                         Timestamp stub_timestamp);

// Replaces the completed prefix of an ongoing episode with a stub, then inserts a new ongoing
// episode immediately after it for the remaining tail messages. The split point references the
// first message of the remaining segment and may point to either a user or assistant message.
[[nodiscard]] absl::Status SplitOngoingEpisodeWithStub(Conversation& conversation,
                                                       std::size_t conversation_item_index,
                                                       std::size_t split_before_message_index,
                                                       std::string stub_text,
                                                       Timestamp stub_timestamp);

// Validates a multi-boundary segmentation plan for one ongoing episode. Every completed segment
// must contain at least 2 messages, and the final segment must also contain at least 2 messages
// when tail_complete is true. Each boundary references the first message of a later segment.
[[nodiscard]] absl::Status
ValidateOngoingEpisodeBoundaryPlan(const OngoingEpisode& ongoing_episode,
                                   const OngoingEpisodeBoundaryPlan& boundary_plan);

// Converts a validated boundary plan into completed message ranges in chronological order. The
// returned ranges use half-open [begin, end) indexing into the original ongoing episode.
[[nodiscard]] absl::StatusOr<std::vector<OngoingEpisodeMessageRange>>
BuildCompletedEpisodeRanges(const OngoingEpisode& ongoing_episode,
                            const OngoingEpisodeBoundaryPlan& boundary_plan);

// Copies one half-open [begin, end) message range out of an ongoing episode.
[[nodiscard]] absl::StatusOr<OngoingEpisode>
SliceOngoingEpisodeMessages(const OngoingEpisode& ongoing_episode, std::size_t begin_message_index,
                            std::size_t end_message_index);

} // namespace isla::server::memory
