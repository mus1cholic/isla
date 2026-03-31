#include "isla/server/memory/llm_json_utils.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace isla::server::memory {

std::string StripMarkdownCodeFences(std::string_view text) {
    // Trim a local copy to detect fences, but return the original if no fences are found.
    std::string_view trimmed = text;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.remove_suffix(1);
    }

    // Must start and end with ``` to be considered a code fence.
    if (!trimmed.starts_with("```") || !trimmed.ends_with("```")) {
        return std::string(text);
    }

    // Strip the closing fence.
    std::string_view inner = trimmed.substr(3, trimmed.size() - 6);

    // Strip an optional language tag on the opening fence (e.g. "json").
    if (const auto newline = inner.find('\n'); newline != std::string_view::npos) {
        // Multi-line: tag ends at the first newline.
        inner.remove_prefix(newline + 1);
    } else {
        // Single-line: strip an optional alphabetic tag and any whitespace that
        // follows it (e.g. ```json{"a":1}``` or ```json {"a":1}```).
        while (!inner.empty() && std::isalpha(static_cast<unsigned char>(inner.front()))) {
            inner.remove_prefix(1);
        }
        while (!inner.empty() && inner.front() == ' ') {
            inner.remove_prefix(1);
        }
    }

    // Trim trailing whitespace/newlines that precede the closing ```.
    while (!inner.empty() && std::isspace(static_cast<unsigned char>(inner.back()))) {
        inner.remove_suffix(1);
    }

    return std::string(inner);
}

} // namespace isla::server::memory
