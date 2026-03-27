#include "isla/shared/ai_gateway_protocol.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include <nlohmann/json.hpp>

namespace isla::shared::ai_gateway {
namespace {

using json = nlohmann::json;

struct MessageTypeEntry {
    std::string_view name;
    MessageType type;
};

constexpr std::array<MessageTypeEntry, 13> kMessageTypeEntries = { {
    { .name = "session.start", .type = MessageType::SessionStart },
    { .name = "session.started", .type = MessageType::SessionStarted },
    { .name = "session.end", .type = MessageType::SessionEnd },
    { .name = "session.ended", .type = MessageType::SessionEnded },
    { .name = "transcript.seed", .type = MessageType::TranscriptSeed },
    { .name = "transcript.seeded", .type = MessageType::TranscriptSeeded },
    { .name = "text.input", .type = MessageType::TextInput },
    { .name = "text.output", .type = MessageType::TextOutput },
    { .name = "audio.output", .type = MessageType::AudioOutput },
    { .name = "turn.completed", .type = MessageType::TurnCompleted },
    { .name = "turn.cancel", .type = MessageType::TurnCancel },
    { .name = "turn.cancelled", .type = MessageType::TurnCancelled },
    { .name = "error", .type = MessageType::Error },
} };

template <typename... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

absl::Status make_parse_error(std::string_view detail) {
    return absl::InvalidArgumentError("AI gateway message parse failed: " + std::string(detail));
}

const json* find_key(const json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return nullptr;
    }
    return &(*it);
}

std::optional<std::string> read_required_string(const json& object, std::string_view key) {
    const json* value = find_key(object, key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    auto parsed = value->get<std::string>();
    if (parsed.empty()) {
        return std::nullopt;
    }
    return parsed;
}

absl::StatusOr<std::optional<std::string>> read_optional_nonempty_string(const json& object,
                                                                         std::string_view key) {
    const json* value = find_key(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        return make_parse_error(std::string(key) + " must be a string when present");
    }
    auto parsed = value->get<std::string>();
    if (parsed.empty()) {
        return make_parse_error(std::string(key) + " must be non-empty when present");
    }
    return parsed;
}

json serialize_message(const GatewayMessage& message) {
    return std::visit(
        Overloaded{
            [](const SessionStartMessage& value) {
                json object = { { "type", message_type_name(MessageType::SessionStart) },
                                { "user_id", value.user_id } };
                if (value.client_session_id.has_value() && !value.client_session_id->empty()) {
                    object["client_session_id"] = *value.client_session_id;
                }
                if (value.session_start_time.has_value() && !value.session_start_time->empty()) {
                    object["session_start_time"] = *value.session_start_time;
                }
                if (value.evaluation_reference_time.has_value() &&
                    !value.evaluation_reference_time->empty()) {
                    object["evaluation_reference_time"] = *value.evaluation_reference_time;
                }
                return object;
            },
            [](const SessionStartedMessage& value) {
                return json{ { "type", message_type_name(MessageType::SessionStarted) },
                             { "session_id", value.session_id } };
            },
            [](const SessionEndMessage& value) {
                return json{ { "type", message_type_name(MessageType::SessionEnd) },
                             { "session_id", value.session_id } };
            },
            [](const SessionEndedMessage& value) {
                return json{ { "type", message_type_name(MessageType::SessionEnded) },
                             { "session_id", value.session_id } };
            },
            [](const TranscriptSeedMessage& value) {
                json object = { { "type", message_type_name(MessageType::TranscriptSeed) },
                                { "turn_id", value.turn_id },
                                { "role", value.role },
                                { "text", value.text } };
                if (value.create_time.has_value() && !value.create_time->empty()) {
                    object["create_time"] = *value.create_time;
                }
                return object;
            },
            [](const TranscriptSeededMessage& value) {
                return json{ { "type", message_type_name(MessageType::TranscriptSeeded) },
                             { "turn_id", value.turn_id },
                             { "role", value.role } };
            },
            [](const TextInputMessage& value) {
                json object = { { "type", message_type_name(MessageType::TextInput) },
                                { "turn_id", value.turn_id },
                                { "text", value.text } };
                if (value.create_time.has_value() && !value.create_time->empty()) {
                    object["create_time"] = *value.create_time;
                }
                return object;
            },
            [](const TextOutputMessage& value) {
                return json{ { "type", message_type_name(MessageType::TextOutput) },
                             { "turn_id", value.turn_id },
                             { "text", value.text } };
            },
            [](const AudioOutputMessage& value) {
                return json{ { "type", message_type_name(MessageType::AudioOutput) },
                             { "turn_id", value.turn_id },
                             { "mime_type", value.mime_type },
                             { "audio_base64", value.audio_base64 } };
            },
            [](const TurnCompletedMessage& value) {
                return json{ { "type", message_type_name(MessageType::TurnCompleted) },
                             { "turn_id", value.turn_id } };
            },
            [](const TurnCancelMessage& value) {
                return json{ { "type", message_type_name(MessageType::TurnCancel) },
                             { "turn_id", value.turn_id } };
            },
            [](const TurnCancelledMessage& value) {
                return json{ { "type", message_type_name(MessageType::TurnCancelled) },
                             { "turn_id", value.turn_id } };
            },
            [](const ErrorMessage& value) {
                json object = { { "type", message_type_name(MessageType::Error) },
                                { "code", value.code },
                                { "message", value.message } };
                if (value.session_id.has_value() && !value.session_id->empty()) {
                    object["session_id"] = *value.session_id;
                }
                if (value.turn_id.has_value() && !value.turn_id->empty()) {
                    object["turn_id"] = *value.turn_id;
                }
                return object;
            },
        },
        message);
}

absl::StatusOr<GatewayMessage> parse_message_object(const json& root, MessageType type) {
    switch (type) {
    case MessageType::SessionStart: {
        auto user_id = read_required_string(root, "user_id");
        if (!user_id.has_value()) {
            return make_parse_error("session.start requires non-empty user_id");
        }
        absl::StatusOr<std::optional<std::string>> session_start_time =
            read_optional_nonempty_string(root, "session_start_time");
        if (!session_start_time.ok()) {
            return session_start_time.status();
        }
        absl::StatusOr<std::optional<std::string>> evaluation_reference_time =
            read_optional_nonempty_string(root, "evaluation_reference_time");
        if (!evaluation_reference_time.ok()) {
            return evaluation_reference_time.status();
        }
        return SessionStartMessage{
            .user_id = std::move(*user_id),
            .client_session_id = read_required_string(root, "client_session_id"),
            .session_start_time = std::move(*session_start_time),
            .evaluation_reference_time = std::move(*evaluation_reference_time),
        };
    }
    case MessageType::SessionStarted: {
        auto session_id = read_required_string(root, "session_id");
        if (!session_id.has_value()) {
            return make_parse_error("session.started requires non-empty session_id");
        }
        return SessionStartedMessage{ std::move(*session_id) };
    }
    case MessageType::SessionEnd: {
        auto session_id = read_required_string(root, "session_id");
        if (!session_id.has_value()) {
            return make_parse_error("session.end requires non-empty session_id");
        }
        return SessionEndMessage{ std::move(*session_id) };
    }
    case MessageType::SessionEnded: {
        auto session_id = read_required_string(root, "session_id");
        if (!session_id.has_value()) {
            return make_parse_error("session.ended requires non-empty session_id");
        }
        return SessionEndedMessage{ std::move(*session_id) };
    }
    case MessageType::TranscriptSeed: {
        auto turn_id = read_required_string(root, "turn_id");
        auto role = read_required_string(root, "role");
        auto text = read_required_string(root, "text");
        if (!turn_id.has_value() || !role.has_value() || !text.has_value()) {
            return make_parse_error("transcript.seed requires non-empty turn_id, role, and text");
        }
        absl::StatusOr<std::optional<std::string>> create_time =
            read_optional_nonempty_string(root, "create_time");
        if (!create_time.ok()) {
            return create_time.status();
        }
        return TranscriptSeedMessage{
            .turn_id = std::move(*turn_id),
            .role = std::move(*role),
            .text = std::move(*text),
            .create_time = std::move(*create_time),
        };
    }
    case MessageType::TranscriptSeeded: {
        auto turn_id = read_required_string(root, "turn_id");
        auto role = read_required_string(root, "role");
        if (!turn_id.has_value() || !role.has_value()) {
            return make_parse_error("transcript.seeded requires non-empty turn_id and role");
        }
        return TranscriptSeededMessage{
            .turn_id = std::move(*turn_id),
            .role = std::move(*role),
        };
    }
    case MessageType::TextInput: {
        auto turn_id = read_required_string(root, "turn_id");
        auto text = read_required_string(root, "text");
        if (!turn_id.has_value() || !text.has_value()) {
            return make_parse_error("text.input requires non-empty turn_id and text");
        }
        absl::StatusOr<std::optional<std::string>> create_time =
            read_optional_nonempty_string(root, "create_time");
        if (!create_time.ok()) {
            return create_time.status();
        }
        return TextInputMessage{
            .turn_id = std::move(*turn_id),
            .text = std::move(*text),
            .create_time = std::move(*create_time),
        };
    }
    case MessageType::TextOutput: {
        auto turn_id = read_required_string(root, "turn_id");
        auto text = read_required_string(root, "text");
        if (!turn_id.has_value() || !text.has_value()) {
            return make_parse_error("text.output requires non-empty turn_id and text");
        }
        return TextOutputMessage{ .turn_id = std::move(*turn_id), .text = std::move(*text) };
    }
    case MessageType::AudioOutput: {
        auto turn_id = read_required_string(root, "turn_id");
        auto mime_type = read_required_string(root, "mime_type");
        auto audio_base64 = read_required_string(root, "audio_base64");
        if (!turn_id.has_value() || !mime_type.has_value() || !audio_base64.has_value()) {
            return make_parse_error(
                "audio.output requires non-empty turn_id, mime_type, and audio_base64");
        }
        return AudioOutputMessage{
            .turn_id = std::move(*turn_id),
            .mime_type = std::move(*mime_type),
            .audio_base64 = std::move(*audio_base64),
        };
    }
    case MessageType::TurnCompleted: {
        auto turn_id = read_required_string(root, "turn_id");
        if (!turn_id.has_value()) {
            return make_parse_error("turn.completed requires non-empty turn_id");
        }
        return TurnCompletedMessage{ std::move(*turn_id) };
    }
    case MessageType::TurnCancel: {
        auto turn_id = read_required_string(root, "turn_id");
        if (!turn_id.has_value()) {
            return make_parse_error("turn.cancel requires non-empty turn_id");
        }
        return TurnCancelMessage{ std::move(*turn_id) };
    }
    case MessageType::TurnCancelled: {
        auto turn_id = read_required_string(root, "turn_id");
        if (!turn_id.has_value()) {
            return make_parse_error("turn.cancelled requires non-empty turn_id");
        }
        return TurnCancelledMessage{ std::move(*turn_id) };
    }
    case MessageType::Error: {
        auto code = read_required_string(root, "code");
        auto message = read_required_string(root, "message");
        if (!code.has_value() || !message.has_value()) {
            return make_parse_error("error requires non-empty code and message");
        }
        return ErrorMessage{ .session_id = read_required_string(root, "session_id"),
                             .turn_id = read_required_string(root, "turn_id"),
                             .code = std::move(*code),
                             .message = std::move(*message) };
    }
    }

    return make_parse_error("message type is unsupported");
}

} // namespace

const char* message_type_name(MessageType type) {
    for (const MessageTypeEntry& entry : kMessageTypeEntries) {
        if (entry.type == type) {
            return entry.name.data();
        }
    }
    return "unknown";
}

std::optional<MessageType> parse_message_type(std::string_view type_name) {
    for (const MessageTypeEntry& entry : kMessageTypeEntries) {
        if (entry.name == type_name) {
            return entry.type;
        }
    }
    return std::nullopt;
}

MessageType message_type(const GatewayMessage& message) {
    return std::visit(
        Overloaded{
            [](const SessionStartMessage&) { return MessageType::SessionStart; },
            [](const SessionStartedMessage&) { return MessageType::SessionStarted; },
            [](const SessionEndMessage&) { return MessageType::SessionEnd; },
            [](const SessionEndedMessage&) { return MessageType::SessionEnded; },
            [](const TranscriptSeedMessage&) { return MessageType::TranscriptSeed; },
            [](const TranscriptSeededMessage&) { return MessageType::TranscriptSeeded; },
            [](const TextInputMessage&) { return MessageType::TextInput; },
            [](const TextOutputMessage&) { return MessageType::TextOutput; },
            [](const AudioOutputMessage&) { return MessageType::AudioOutput; },
            [](const TurnCompletedMessage&) { return MessageType::TurnCompleted; },
            [](const TurnCancelMessage&) { return MessageType::TurnCancel; },
            [](const TurnCancelledMessage&) { return MessageType::TurnCancelled; },
            [](const ErrorMessage&) { return MessageType::Error; },
        },
        message);
}

std::string to_json_string(const GatewayMessage& message) {
    return serialize_message(message).dump();
}

absl::StatusOr<GatewayMessage> parse_json_message(std::string_view json_text) {
    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return make_parse_error("invalid JSON (" + std::string(e.what()) + ")");
    }

    if (!root.is_object()) {
        return make_parse_error("top-level JSON value must be an object");
    }

    const auto type_name = read_required_string(root, "type");
    if (!type_name.has_value()) {
        return make_parse_error("missing non-empty type");
    }

    const auto parsed_type = parse_message_type(*type_name);
    if (!parsed_type.has_value()) {
        return make_parse_error("unknown type '" + *type_name + "'");
    }

    return parse_message_object(root, *parsed_type);
}

} // namespace isla::shared::ai_gateway
