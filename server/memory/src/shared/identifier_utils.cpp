#include "isla/server/memory/identifier_utils.hpp"

#include <string>
#include <string_view>

#include "absl/strings/ascii.h"

namespace isla::server::memory {

std::string NormalizeIdentifierComponent(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool last_was_separator = true;
    for (const unsigned char ch : text) {
        if (absl::ascii_isalnum(ch)) {
            normalized.push_back(absl::ascii_tolower(ch));
            last_was_separator = false;
            continue;
        }
        if (!last_was_separator) {
            normalized.push_back('_');
            last_was_separator = true;
        }
    }
    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    return normalized;
}

} // namespace isla::server::memory
