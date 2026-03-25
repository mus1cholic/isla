#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "absl/status/statusor.h"

namespace isla::shared::ai_gateway {

enum class MessageType {
    SessionStart = 0,
    SessionStarted,
    SessionEnd,
    SessionEnded,
    TranscriptSeed,
    TranscriptSeeded,
    TextInput,
    TextOutput,
    AudioOutput,
    TurnCompleted,
    TurnCancel,
    TurnCancelled,
    Error,
};

struct SessionStartMessage {
    std::optional<std::string> client_session_id;
};

struct SessionStartedMessage {
    std::string session_id;
};

struct SessionEndMessage {
    std::string session_id;
};

struct SessionEndedMessage {
    std::string session_id;
};

struct TranscriptSeedMessage {
    std::string turn_id;
    std::string role;
    std::string text;
};

struct TranscriptSeededMessage {
    std::string turn_id;
    std::string role;
};

struct TextInputMessage {
    std::string turn_id;
    std::string text;
};

struct TextOutputMessage {
    std::string turn_id;
    std::string text;
};

struct AudioOutputMessage {
    std::string turn_id;
    std::string mime_type;
    std::string audio_base64;
};

struct TurnCompletedMessage {
    std::string turn_id;
};

struct TurnCancelMessage {
    std::string turn_id;
};

struct TurnCancelledMessage {
    std::string turn_id;
};

struct ErrorMessage {
    std::optional<std::string> session_id;
    std::optional<std::string> turn_id;
    std::string code;
    std::string message;
};

using GatewayMessage =
    std::variant<SessionStartMessage, SessionStartedMessage, SessionEndMessage, SessionEndedMessage,
                 TranscriptSeedMessage, TranscriptSeededMessage, TextInputMessage,
                 TextOutputMessage, AudioOutputMessage, TurnCompletedMessage, TurnCancelMessage,
                 TurnCancelledMessage, ErrorMessage>;

[[nodiscard]] const char* message_type_name(MessageType type);
[[nodiscard]] std::optional<MessageType> parse_message_type(std::string_view type_name);
[[nodiscard]] MessageType message_type(const GatewayMessage& message);
[[nodiscard]] std::string to_json_string(const GatewayMessage& message);
[[nodiscard]] absl::StatusOr<GatewayMessage> parse_json_message(std::string_view json_text);

} // namespace isla::shared::ai_gateway
