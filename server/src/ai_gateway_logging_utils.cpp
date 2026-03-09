#include "isla/server/ai_gateway_logging_utils.hpp"

#include <sstream>

namespace isla::server::ai_gateway {

std::string SanitizeForLog(std::string_view value) {
    std::ostringstream escaped;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                escaped << "\\x";
                constexpr char kHexDigits[] = "0123456789ABCDEF";
                escaped << kHexDigits[(ch >> 4) & 0x0F] << kHexDigits[ch & 0x0F];
            } else {
                escaped << static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped.str();
}

} // namespace isla::server::ai_gateway
