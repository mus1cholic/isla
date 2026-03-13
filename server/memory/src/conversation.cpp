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
        LOG(WARNING) << "Conversation flush rejected: item index out of range index="
                     << conversation_item_index << " item_count=" << conversation.items.size();
        return absl::InvalidArgumentError("flush target exceeds conversation size");
    }
    if (stub_text.empty()) {
        LOG(WARNING) << "Conversation flush rejected: empty stub text index="
                     << conversation_item_index;
        return absl::InvalidArgumentError("flush stub text must be non-empty");
    }

    auto& item = conversation.items[conversation_item_index];
    if (item.type != ConversationItemType::OngoingEpisode || !item.ongoing_episode.has_value()) {
        LOG(WARNING) << "Conversation flush rejected: target is not an ongoing episode index="
                     << conversation_item_index;
        return absl::InvalidArgumentError("flush target must be an ongoing episode");
    }
    if (item.ongoing_episode->messages.empty()) {
        LOG(WARNING) << "Conversation flush rejected: target ongoing episode is empty index="
                     << conversation_item_index;
        return absl::InvalidArgumentError("flush target must contain at least one message");
    }

    const std::size_t message_count = item.ongoing_episode->messages.size();
    item.type = ConversationItemType::EpisodeStub;
    item.ongoing_episode.reset();
    item.episode_stub = EpisodeStub{
        .content = std::move(stub_text),
        .create_time = stub_timestamp,
    };
    LOG(INFO) << "Conversation replaced ongoing episode with stub index=" << conversation_item_index
              << " message_count=" << message_count;
    return absl::OkStatus();
}

} // namespace isla::server::memory
