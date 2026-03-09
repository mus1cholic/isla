#include "isla/server/memory/memory_timestamp_utils.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace isla::server::memory {
namespace {

int ParseFixedDigits(std::string_view text, std::size_t offset, std::size_t count) {
    if (offset + count > text.size()) {
        throw std::invalid_argument("timestamp ended unexpectedly");
    }

    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char ch = text[offset + index];
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument("timestamp contains a non-digit");
        }
        value = (value * 10) + (ch - '0');
    }
    return value;
}

void ExpectCharacter(std::string_view text, std::size_t offset, char expected) {
    if (offset >= text.size() || text[offset] != expected) {
        throw std::invalid_argument("timestamp is not valid ISO-8601");
    }
}

} // namespace

Timestamp ParseTimestamp(std::string_view text) {
    const int year = ParseFixedDigits(text, 0, 4);
    ExpectCharacter(text, 4, '-');
    const unsigned month = static_cast<unsigned>(ParseFixedDigits(text, 5, 2));
    ExpectCharacter(text, 7, '-');
    const unsigned day = static_cast<unsigned>(ParseFixedDigits(text, 8, 2));
    ExpectCharacter(text, 10, 'T');
    const int hour = ParseFixedDigits(text, 11, 2);
    ExpectCharacter(text, 13, ':');
    const int minute = ParseFixedDigits(text, 14, 2);
    ExpectCharacter(text, 16, ':');
    const int second = ParseFixedDigits(text, 17, 2);

    if (hour > 23 || minute > 59 || second > 59) {
        throw std::invalid_argument("timestamp time component is out of range");
    }

    std::size_t cursor = 19;
    int milliseconds = 0;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        const std::size_t fraction_start = cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == fraction_start) {
            throw std::invalid_argument("timestamp fractional seconds are empty");
        }

        const std::size_t fraction_digits = cursor - fraction_start;
        const std::size_t digits_to_use = std::min<std::size_t>(fraction_digits, 3);
        milliseconds = ParseFixedDigits(text, fraction_start, digits_to_use);
        for (std::size_t index = digits_to_use; index < 3; ++index) {
            milliseconds *= 10;
        }
    }

    std::chrono::minutes utc_offset{ 0 };
    if (cursor >= text.size()) {
        throw std::invalid_argument("timestamp is missing timezone information");
    }
    if (text[cursor] == 'Z') {
        ++cursor;
    } else if (text[cursor] == '+' || text[cursor] == '-') {
        const bool negative_offset = text[cursor] == '-';
        ++cursor;
        const int offset_hours = ParseFixedDigits(text, cursor, 2);
        cursor += 2;
        ExpectCharacter(text, cursor, ':');
        ++cursor;
        const int offset_minutes = ParseFixedDigits(text, cursor, 2);
        cursor += 2;
        if (offset_hours > 23 || offset_minutes > 59) {
            throw std::invalid_argument("timestamp timezone offset is out of range");
        }

        utc_offset = std::chrono::hours{ offset_hours } + std::chrono::minutes{ offset_minutes };
        if (negative_offset) {
            utc_offset = -utc_offset;
        }
    } else {
        throw std::invalid_argument("timestamp timezone must be Z or an offset");
    }

    if (cursor != text.size()) {
        throw std::invalid_argument("timestamp has trailing characters");
    }

    const std::chrono::year_month_day ymd{ std::chrono::year{ year } / std::chrono::month{ month } /
                                           std::chrono::day{ day } };
    if (!ymd.ok()) {
        throw std::invalid_argument("timestamp date component is out of range");
    }

    const auto local_time = std::chrono::sys_days{ ymd } + std::chrono::hours{ hour } +
                            std::chrono::minutes{ minute } + std::chrono::seconds{ second } +
                            std::chrono::milliseconds{ milliseconds };
    return local_time - utc_offset;
}

std::string FormatTimestamp(Timestamp timestamp) {
    const auto days = std::chrono::floor<std::chrono::days>(timestamp);
    const std::chrono::year_month_day ymd{ days };
    const std::chrono::hh_mm_ss time_of_day{ timestamp - days };

    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-'
           << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
           << static_cast<unsigned>(ymd.day()) << 'T' << std::setw(2) << time_of_day.hours().count()
           << ':' << std::setw(2) << time_of_day.minutes().count() << ':' << std::setw(2)
           << time_of_day.seconds().count();
    if (time_of_day.subseconds().count() != 0) {
        stream << '.' << std::setw(3) << time_of_day.subseconds().count();
    }
    stream << 'Z';
    return stream.str();
}

} // namespace isla::server::memory

namespace nlohmann {

void adl_serializer<isla::server::memory::Timestamp>::to_json(
    json& j, const isla::server::memory::Timestamp& value) {
    j = isla::server::memory::FormatTimestamp(value);
}

void adl_serializer<isla::server::memory::Timestamp>::from_json(
    const json& j, isla::server::memory::Timestamp& value) {
    if (!j.is_string()) {
        throw std::invalid_argument("timestamp JSON value must be a string");
    }
    value = isla::server::memory::ParseTimestamp(j.get<std::string>());
}

} // namespace nlohmann
