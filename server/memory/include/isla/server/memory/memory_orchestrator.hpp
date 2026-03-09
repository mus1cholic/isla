#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/working_memory.hpp"

namespace isla::server::memory {

struct GatewayTurnText {
    std::string session_id;
    std::string turn_id;
    std::string text;
    Timestamp create_time;
};

struct GatewayUserQuery : GatewayTurnText {
    GatewayUserQuery() = default;
    GatewayUserQuery(std::string session_id_in, std::string turn_id_in, std::string text_in,
                     Timestamp create_time_in)
        : GatewayTurnText{
              .session_id = std::move(session_id_in),
              .turn_id = std::move(turn_id_in),
              .text = std::move(text_in),
              .create_time = create_time_in,
          } {}
};

struct GatewayAssistantReply : GatewayTurnText {
    GatewayAssistantReply() = default;
    GatewayAssistantReply(std::string session_id_in, std::string turn_id_in, std::string text_in,
                          Timestamp create_time_in)
        : GatewayTurnText{
              .session_id = std::move(session_id_in),
              .turn_id = std::move(turn_id_in),
              .text = std::move(text_in),
              .create_time = create_time_in,
          } {}
};

// Central entry point for gateway-delivered user turns. The gateway only forwards the raw user
// query; this handler is responsible for converting it into working-memory state changes and
// coordinating future mid/long-term memory hooks.
class MemoryOrchestrator {
  public:
    MemoryOrchestrator(std::string session_id, WorkingMemory memory);

    [[nodiscard]] static MemoryOrchestrator Create(std::string session_id,
                                                   const WorkingMemoryInit& init);

    [[nodiscard]] absl::Status HandleUserQuery(const GatewayUserQuery& query);
    [[nodiscard]] absl::Status HandleAssistantReply(const GatewayAssistantReply& reply);
    [[nodiscard]] absl::Status
    ApplyCompletedEpisodeFlush(const CompletedOngoingEpisodeFlush& flush);
    [[nodiscard]] absl::StatusOr<std::string> RenderPrompt() const;

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    [[nodiscard]] const WorkingMemory& memory() const {
        return memory_;
    }

    [[nodiscard]] WorkingMemory& mutable_memory() {
        return memory_;
    }

  private:
    [[nodiscard]] absl::Status ValidateTurnText(const GatewayTurnText& turn_text,
                                                std::string_view role_label) const;
    [[nodiscard]] absl::Status HandleConversationMessage(std::string_view session_id,
                                                         std::string_view turn_id,
                                                         std::string_view text,
                                                         Timestamp create_time, MessageRole role);
    [[nodiscard]] absl::Status AfterUserQueryAppended(const Message& user_message);
    [[nodiscard]] absl::Status AfterAssistantReplyAppended(const Message& assistant_message);
    [[nodiscard]] absl::StatusOr<std::optional<RetrievedMemory>>
    RetrieveRelevantMemories(const Message& user_message);
    [[nodiscard]] absl::StatusOr<std::optional<OngoingEpisodeFlushCandidate>>
    MaybeCaptureFlushCandidate(const Message& user_message);

    std::string session_id_;
    WorkingMemory memory_;
};

} // namespace isla::server::memory
