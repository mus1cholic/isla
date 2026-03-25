#include "isla/server/memory/llm_json_utils.hpp"

#include <string>
#include <string_view>

namespace isla::server::memory {

std::string StripMarkdownCodeFences(std::string_view text) {
    // Trim leading/trailing whitespace so that a response like
    // "  ```json\n{}\n```  " is still handled.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }

    // Must start with ``` to be considered a code fence.
    if (!text.starts_with("```")) {
        return std::string(text);
    }

    // Must end with ``` as well.
    if (!text.ends_with("```")) {
        return std::string(text);
    }

    // Strip the closing fence.
    std::string_view inner = text.substr(3, text.size() - 6);

    // Strip an optional language tag on the opening fence (e.g. "json").
    // The tag ends at the first newline.
    if (const auto newline = inner.find('\n'); newline != std::string_view::npos) {
        inner.remove_prefix(newline + 1);
    } else {
        // Single-line: ```{...}``` — just strip the fences, no tag to skip.
    }

    // Trim trailing whitespace/newlines that precede the closing ```.
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t' || inner.back() == '\r' ||
                              inner.back() == '\n')) {
        inner.remove_suffix(1);
    }

    return std::string(inner);
}

} // namespace isla::server::memory
