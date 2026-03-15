#pragma once

#include <string_view>

namespace isla::server::ai_gateway {

enum class OpenAiReasoningEffort {
    kNone = 0,
    kMinimal,
    kLow,
    kMedium,
    kHigh,
    kXHigh,
};

[[nodiscard]] constexpr std::string_view
OpenAiReasoningEffortToString(OpenAiReasoningEffort effort) {
    switch (effort) {
    case OpenAiReasoningEffort::kNone:
        return "none";
    case OpenAiReasoningEffort::kMinimal:
        return "minimal";
    case OpenAiReasoningEffort::kLow:
        return "low";
    case OpenAiReasoningEffort::kMedium:
        return "medium";
    case OpenAiReasoningEffort::kHigh:
        return "high";
    case OpenAiReasoningEffort::kXHigh:
        return "xhigh";
    }
    return "unknown";
}

} // namespace isla::server::ai_gateway
