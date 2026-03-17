#pragma once

#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

void BeginOngoingEpisode(Conversation& conversation);
void AppendUserMessage(Conversation& conversation, std::string content, Timestamp create_time);
void AppendAssistantMessage(Conversation& conversation, std::string content, Timestamp create_time);
void AppendEpisodeStub(Conversation& conversation, std::string content, Timestamp create_time);

[[nodiscard]] absl::Status ReplaceOngoingEpisodeWithStub(Conversation& conversation,
                                                         std::size_t conversation_item_index,
                                                         std::string stub_text,
                                                         Timestamp stub_timestamp);

[[nodiscard]] absl::Status SplitOngoingEpisodeWithStub(Conversation& conversation,
                                                       std::size_t conversation_item_index,
                                                       std::size_t split_at_message_index,
                                                       std::string stub_text,
                                                       Timestamp stub_timestamp);

} // namespace isla::server::memory
