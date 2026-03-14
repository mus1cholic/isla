#include "isla/server/openai_responses_http_utils.hpp"

#include <algorithm>
#include <charconv>

#include <nlohmann/json.hpp>
#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_responses_json_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

bool ContainsForbiddenHttpFieldChar(std::string_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](char ch) { return ch == '\r' || ch == '\n' || ch == '\0'; });
}

bool ContainsAsciiWhitespaceOrControl(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        return byte <= 0x20U || byte == 0x7fU;
    });
}

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

absl::Status ValidateHttpFieldValue(std::string_view field_name, std::string_view value) {
    if (ContainsForbiddenHttpFieldChar(value)) {
        return invalid_argument(std::string(field_name) +
                                " must not contain carriage return, newline, or NUL");
    }
    return absl::OkStatus();
}

absl::Status ValidateHttpHostValue(std::string_view value) {
    if (absl::Status status = ValidateHttpFieldValue("openai responses host", value);
        !status.ok()) {
        return status;
    }
    if (ContainsAsciiWhitespaceOrControl(value)) {
        return invalid_argument(
            "openai responses host must not contain ASCII whitespace or control characters");
    }
    return absl::OkStatus();
}

absl::Status ValidateHttpTargetValue(std::string_view value) {
    if (absl::Status status = ValidateHttpFieldValue("openai responses target", value);
        !status.ok()) {
        return status;
    }
    if (ContainsAsciiWhitespaceOrControl(value)) {
        return invalid_argument(
            "openai responses target must not contain ASCII whitespace or control characters");
    }
    return absl::OkStatus();
}

std::optional<unsigned int> ParseHttpStatusCode(std::string_view header_text) {
    std::optional<unsigned int> status_code;
    std::size_t cursor = 0;
    while (cursor <= header_text.size()) {
        const std::size_t next_newline = header_text.find('\n', cursor);
        const std::size_t line_end =
            next_newline == std::string_view::npos ? header_text.size() : next_newline;
        std::string line(header_text.substr(cursor, line_end - cursor));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("HTTP/", 0) == 0) {
            const std::size_t first_space = line.find(' ');
            const std::size_t second_space = first_space == std::string::npos
                                                 ? std::string::npos
                                                 : line.find(' ', first_space + 1U);
            if (first_space != std::string::npos) {
                const std::string_view code_text = std::string_view(line).substr(
                    first_space + 1U,
                    (second_space == std::string::npos ? line.size() : second_space) -
                        (first_space + 1U));
                unsigned int parsed_status_code = 0;
                const auto parse_result = std::from_chars(
                    code_text.data(), code_text.data() + code_text.size(), parsed_status_code);
                if (parse_result.ec == std::errc() &&
                    parse_result.ptr == code_text.data() + code_text.size()) {
                    status_code = parsed_status_code;
                }
            }
        }

        if (next_newline == std::string_view::npos) {
            break;
        }
        cursor = next_newline + 1U;
    }
    return status_code;
}

absl::Status MapHttpErrorStatus(unsigned int status_code, std::string message) {
    switch (status_code) {
    case 400:
    case 404:
    case 409:
    case 422:
        return absl::InvalidArgumentError(std::move(message));
    case 401:
        return absl::UnauthenticatedError(std::move(message));
    case 403:
        return absl::PermissionDeniedError(std::move(message));
    case 408:
    case 504:
        return absl::DeadlineExceededError(std::move(message));
    case 429:
    case 500:
    case 502:
    case 503:
        return absl::UnavailableError(std::move(message));
    default:
        return absl::InternalError(std::move(message));
    }
}

std::string BuildHttpErrorMessage(unsigned int status_code, std::string_view body) {
    std::string message = "openai responses request failed";
    if (!body.empty()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(body);
            const std::string extracted = ExtractJsonErrorMessage(parsed);
            if (!extracted.empty()) {
                return extracted;
            }
        } catch (const std::exception& error) {
            VLOG(1) << "AI gateway openai responses error body was not JSON detail='"
                    << SanitizeForLog(error.what()) << "'";
        }
    }
    return message + " with status " + std::to_string(status_code);
}

} // namespace isla::server::ai_gateway
