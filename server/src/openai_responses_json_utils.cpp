#include "isla/server/openai_responses_json_utils.hpp"

namespace isla::server::ai_gateway {

absl::StatusOr<std::optional<std::string>> ReadOptionalStringField(const nlohmann::json& object,
                                                                   std::string_view field_name) {
    const auto it = object.find(std::string(field_name));
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        return absl::InternalError("openai responses event field '" + std::string(field_name) +
                                   "' must be a string");
    }
    return it->get<std::string>();
}

std::string ExtractJsonErrorMessage(const nlohmann::json& json) {
    if (json.contains("error") && json["error"].is_object()) {
        const auto& error = json["error"];
        const absl::StatusOr<std::optional<std::string>> message =
            ReadOptionalStringField(error, "message");
        if (message.ok() && message->has_value()) {
            return **message;
        }
    }
    const absl::StatusOr<std::optional<std::string>> message =
        ReadOptionalStringField(json, "message");
    if (message.ok() && message->has_value()) {
        return **message;
    }
    return {};
}

} // namespace isla::server::ai_gateway
