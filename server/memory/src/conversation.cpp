#include "isla/server/memory/conversation.hpp"

#include <string>
#include <utility>

#include "absl/log/log.h"

namespace isla::server::memory {
namespace {

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
        LOG(WARNING)
            << "Conversation ReplaceOngoingEpisodeWithStub rejected because the requested "
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
        LOG(WARNING)
            << "Conversation ReplaceOngoingEpisodeWithStub rejected because the target "
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

} // namespace isla::server::memory
