#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace isla::server::memory {

using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

// Parses an ISO-8601 timestamp with timezone information and normalizes it into UTC millisecond
// precision. Fractional seconds beyond milliseconds are rejected instead of rounded.
[[nodiscard]] Timestamp ParseTimestamp(std::string_view text);

// Formats a timestamp as an ISO-8601 UTC string using 'Z', omitting the fractional component when
// it is exactly zero.
[[nodiscard]] std::string FormatTimestamp(Timestamp timestamp);

} // namespace isla::server::memory

namespace nlohmann {

template <> struct adl_serializer<isla::server::memory::Timestamp> {
    // Serializes Timestamp values using the module's canonical ISO-8601 UTC string format.
    static void to_json(json& j, const isla::server::memory::Timestamp& value);
    // Deserializes Timestamp values from JSON strings and reuses ParseTimestamp validation rules.
    static void from_json(const json& j, isla::server::memory::Timestamp& value);
};

} // namespace nlohmann
