#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace isla::server::memory {

using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

[[nodiscard]] Timestamp ParseTimestamp(std::string_view text);
[[nodiscard]] std::string FormatTimestamp(Timestamp timestamp);

} // namespace isla::server::memory

namespace nlohmann {

template <> struct adl_serializer<isla::server::memory::Timestamp> {
    static void to_json(json& j, const isla::server::memory::Timestamp& value);
    static void from_json(const json& j, isla::server::memory::Timestamp& value);
};

} // namespace nlohmann
