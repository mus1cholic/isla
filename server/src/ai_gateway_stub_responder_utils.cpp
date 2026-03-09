#include "isla/server/ai_gateway_stub_responder_utils.hpp"

#include <chrono>

namespace isla::server::ai_gateway {

std::string BuildStubReply(std::string_view prefix, std::string_view text) {
    std::string reply;
    reply.reserve(prefix.size() + text.size());
    reply.append(prefix);
    reply.append(text);
    return reply;
}

isla::server::memory::Timestamp NowTimestamp() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

} // namespace isla::server::ai_gateway
