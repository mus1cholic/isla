#pragma once

#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

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
// episode immediately after it for the remaining tail messages. The split point must reference a
// user message so the retained tail begins at a fresh user turn.
[[nodiscard]] absl::Status SplitOngoingEpisodeWithStub(Conversation& conversation,
                                                       std::size_t conversation_item_index,
                                                       std::size_t split_at_message_index,
                                                       std::string stub_text,
                                                       Timestamp stub_timestamp);

} // namespace isla::server::memory
