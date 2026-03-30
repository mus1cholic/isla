#include "isla/server/memory/conversation.hpp"

#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace isla::server::memory {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

OngoingEpisode& CurrentOngoingEpisode(Conversation& conversation) {
    if (conversation.items.empty() ||
        conversation.items.back().type != ConversationItemType::OngoingEpisode ||
        !conversation.items.back().ongoing_episode.has_value()) {
        BeginOngoingEpisode(conversation);
    }
    return *conversation.items.back().ongoing_episode;
}

void AppendMessageToCurrentEpisode(Conversation& conversation, MessageRole role,
                                   std::string content, Timestamp create_time) {
    CurrentOngoingEpisode(conversation)
        .messages.push_back(Message{
            .role = role,
            .content = std::move(content),
            .create_time = create_time,
        });
}

} // namespace

void BeginOngoingEpisode(Conversation& conversation) {
    conversation.items.push_back(ConversationItem{
        .type = ConversationItemType::OngoingEpisode,
        .ongoing_episode = OngoingEpisode{ .messages = {} },
        .episode_stub = std::nullopt,
    });
    VLOG(1) << "Conversation began ongoing episode item_count=" << conversation.items.size();
}

void AppendUserMessage(Conversation& conversation, std::string content, Timestamp create_time) {
    AppendMessageToCurrentEpisode(conversation, MessageRole::User, std::move(content), create_time);
}

void AppendAssistantMessage(Conversation& conversation, std::string content,
                            Timestamp create_time) {
    AppendMessageToCurrentEpisode(conversation, MessageRole::Assistant, std::move(content),
                                  create_time);
}

void AppendEpisodeStub(Conversation& conversation, std::string content, Timestamp create_time) {
    conversation.items.push_back(ConversationItem{
        .type = ConversationItemType::EpisodeStub,
        .ongoing_episode = std::nullopt,
        .episode_stub =
            EpisodeStub{
                .content = std::move(content),
                .create_time = create_time,
            },
    });
    VLOG(1) << "Conversation appended episode stub item_count=" << conversation.items.size();
}

absl::Status ReplaceOngoingEpisodeWithStub(Conversation& conversation,
                                           std::size_t conversation_item_index,
                                           std::string stub_text, Timestamp stub_timestamp) {
    if (conversation_item_index >= conversation.items.size()) {
        LOG(WARNING) << "Conversation ReplaceOngoingEpisodeWithStub rejected because the requested "
                        "conversation item index is outside the current conversation"
                     << " conversation_item_index=" << conversation_item_index
                     << " conversation_item_count=" << conversation.items.size();
        return absl::InvalidArgumentError(
            "ReplaceOngoingEpisodeWithStub requires conversation_item_index to reference an "
            "existing conversation item");
    }
    if (stub_text.empty()) {
        LOG(WARNING)
            << "Conversation ReplaceOngoingEpisodeWithStub rejected because episode_stub_content "
               "was empty"
            << " conversation_item_index=" << conversation_item_index;
        return absl::InvalidArgumentError(
            "ReplaceOngoingEpisodeWithStub requires a non-empty episode stub text");
    }

    auto& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        LOG(WARNING) << "Conversation ReplaceOngoingEpisodeWithStub rejected because the target "
                        "conversation item is not an ongoing episode"
                     << " conversation_item_index=" << conversation_item_index
                     << " conversation_item_type="
                     << (item.type == ConversationItemType::OngoingEpisode ? "ongoing_episode"
                                                                           : "episode_stub");
        return absl::InvalidArgumentError(
            "ReplaceOngoingEpisodeWithStub requires the target conversation item to be an "
            "ongoing episode");
    }
    if (item.ongoing_episode->messages.empty()) {
        LOG(WARNING)
            << "Conversation ReplaceOngoingEpisodeWithStub rejected because the target ongoing "
               "episode contains no messages"
            << " conversation_item_index=" << conversation_item_index;
        return absl::InvalidArgumentError(
            "ReplaceOngoingEpisodeWithStub requires the target ongoing episode to contain at "
            "least one message");
    }

    const std::size_t message_count = item.ongoing_episode->messages.size();
    item.type = ConversationItemType::EpisodeStub;
    item.ongoing_episode.reset();
    item.episode_stub = EpisodeStub{
        .content = std::move(stub_text),
        .create_time = stub_timestamp,
    };
    LOG(INFO) << "Conversation ReplaceOngoingEpisodeWithStub replaced the target ongoing episode "
                 "with an episode stub"
              << " conversation_item_index=" << conversation_item_index
              << " replaced_message_count=" << message_count;
    return absl::OkStatus();
}

absl::Status SplitOngoingEpisodeWithStub(Conversation& conversation,
                                         std::size_t conversation_item_index,
                                         std::size_t split_at_message_index, std::string stub_text,
                                         Timestamp stub_timestamp) {
    if (conversation_item_index >= conversation.items.size()) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because the requested "
                        "conversation item index is outside the current conversation"
                     << " conversation_item_index=" << conversation_item_index
                     << " conversation_item_count=" << conversation.items.size();
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires conversation_item_index to reference an "
            "existing conversation item");
    }
    if (stub_text.empty()) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because "
                        "episode_stub_content was empty"
                     << " conversation_item_index=" << conversation_item_index;
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires a non-empty episode stub text");
    }

    auto& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because the target "
                        "conversation item is not an ongoing episode"
                     << " conversation_item_index=" << conversation_item_index
                     << " conversation_item_type="
                     << (item.type == ConversationItemType::OngoingEpisode ? "ongoing_episode"
                                                                           : "episode_stub");
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires the target conversation item to be an "
            "ongoing episode");
    }

    auto& messages = item.ongoing_episode->messages;
    if (split_at_message_index >= messages.size()) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because "
                        "split_at_message_index is out of range"
                     << " conversation_item_index=" << conversation_item_index
                     << " split_at_message_index=" << split_at_message_index
                     << " message_count=" << messages.size();
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires split_at_message_index to be within the "
            "message range");
    }
    if (split_at_message_index < 2) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because "
                        "split_at_message_index is too small to form a complete exchange"
                     << " conversation_item_index=" << conversation_item_index
                     << " split_at_message_index=" << split_at_message_index;
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires at least 2 messages before the split point");
    }
    if (messages[split_at_message_index].role != MessageRole::User) {
        LOG(WARNING) << "Conversation SplitOngoingEpisodeWithStub rejected because the message at "
                        "split_at_message_index is not a user message"
                     << " conversation_item_index=" << conversation_item_index
                     << " split_at_message_index=" << split_at_message_index;
        return absl::InvalidArgumentError(
            "SplitOngoingEpisodeWithStub requires split_at_message_index to reference a user "
            "message");
    }

    // Extract remaining messages [split_at, end) before mutating the item.
    std::vector<Message> remaining_messages(
        std::make_move_iterator(messages.begin() +
                                static_cast<std::ptrdiff_t>(split_at_message_index)),
        std::make_move_iterator(messages.end()));

    const std::size_t completed_count = split_at_message_index;
    const std::size_t remaining_count = remaining_messages.size();

    // Replace the item at the target index with an episode stub.
    item.type = ConversationItemType::EpisodeStub;
    item.ongoing_episode.reset();
    item.episode_stub = EpisodeStub{
        .content = std::move(stub_text),
        .create_time = stub_timestamp,
    };

    // Insert a new ongoing episode immediately after the stub with the remaining messages.
    conversation.items.insert(
        conversation.items.begin() + static_cast<std::ptrdiff_t>(conversation_item_index) + 1,
        ConversationItem{
            .type = ConversationItemType::OngoingEpisode,
            .ongoing_episode = OngoingEpisode{ .messages = std::move(remaining_messages) },
            .episode_stub = std::nullopt,
        });

    LOG(INFO) << "Conversation SplitOngoingEpisodeWithStub split the target ongoing episode"
              << " conversation_item_index=" << conversation_item_index
              << " split_at_message_index=" << split_at_message_index
              << " completed_message_count=" << completed_count
              << " remaining_message_count=" << remaining_count;
    return absl::OkStatus();
}

absl::Status ValidateOngoingEpisodeBoundaryPlan(const OngoingEpisode& ongoing_episode,
                                                const OngoingEpisodeBoundaryPlan& boundary_plan) {
    const auto& messages = ongoing_episode.messages;
    const std::size_t message_count = messages.size();

    if (boundary_plan.boundary_message_indices.empty()) {
        if (!boundary_plan.tail_complete) {
            return absl::OkStatus();
        }
        if (message_count < 2U) {
            return invalid_argument(
                "tail_complete requires the final segment to contain at least 2 messages");
        }
        return absl::OkStatus();
    }

    std::size_t previous_boundary = 0U;
    for (std::size_t boundary_index = 0;
         boundary_index < boundary_plan.boundary_message_indices.size(); ++boundary_index) {
        const std::size_t starts_at = boundary_plan.boundary_message_indices[boundary_index];
        if (starts_at >= message_count) {
            return invalid_argument("boundary starts_at exceeds message count");
        }
        if (boundary_index > 0U && starts_at <= previous_boundary) {
            return invalid_argument("boundaries must be strictly increasing");
        }
        if (messages[starts_at].role != MessageRole::User) {
            return invalid_argument("boundaries must reference user messages");
        }
        if (starts_at - previous_boundary < 2U) {
            return invalid_argument(
                "each completed segment before a boundary must contain at least 2 messages");
        }
        previous_boundary = starts_at;
    }

    const std::size_t final_segment_size = message_count - previous_boundary;
    if (final_segment_size < 2U) {
        return invalid_argument("the final segment after the last boundary must contain at least "
                                "2 messages");
    }

    return absl::OkStatus();
}

absl::StatusOr<std::vector<OngoingEpisodeMessageRange>>
BuildCompletedEpisodeRanges(const OngoingEpisode& ongoing_episode,
                            const OngoingEpisodeBoundaryPlan& boundary_plan) {
    if (absl::Status status = ValidateOngoingEpisodeBoundaryPlan(ongoing_episode, boundary_plan);
        !status.ok()) {
        return status;
    }

    std::vector<OngoingEpisodeMessageRange> ranges;
    std::size_t previous_boundary = 0U;
    for (const std::size_t starts_at : boundary_plan.boundary_message_indices) {
        ranges.push_back(OngoingEpisodeMessageRange{
            .begin_message_index = previous_boundary,
            .end_message_index = starts_at,
        });
        previous_boundary = starts_at;
    }

    if (boundary_plan.tail_complete) {
        ranges.push_back(OngoingEpisodeMessageRange{
            .begin_message_index = previous_boundary,
            .end_message_index = ongoing_episode.messages.size(),
        });
    }

    return ranges;
}

absl::StatusOr<OngoingEpisode> SliceOngoingEpisodeMessages(const OngoingEpisode& ongoing_episode,
                                                           std::size_t begin_message_index,
                                                           std::size_t end_message_index) {
    const auto& messages = ongoing_episode.messages;
    if (begin_message_index > end_message_index) {
        return invalid_argument("message slice begin must be less than or equal to end");
    }
    if (end_message_index > messages.size()) {
        return invalid_argument("message slice end exceeds message count");
    }

    OngoingEpisode slice;
    slice.messages.assign(messages.begin() + static_cast<std::ptrdiff_t>(begin_message_index),
                          messages.begin() + static_cast<std::ptrdiff_t>(end_message_index));
    return slice;
}

} // namespace isla::server::memory
