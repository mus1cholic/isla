#pragma once

#include <optional>
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

[[nodiscard]] constexpr std::optional<std::string_view>
TryOpenAiReasoningEffortToString(OpenAiReasoningEffort effort) {
    switch (effort) {
    case OpenAiReasoningEffort::kNone:
        return std::string_view("none");
    case OpenAiReasoningEffort::kMinimal:
        return std::string_view("minimal");
    case OpenAiReasoningEffort::kLow:
        return std::string_view("low");
    case OpenAiReasoningEffort::kMedium:
        return std::string_view("medium");
    case OpenAiReasoningEffort::kHigh:
        return std::string_view("high");
    case OpenAiReasoningEffort::kXHigh:
        return std::string_view("xhigh");
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<OpenAiReasoningEffort>
TryParseOpenAiReasoningEffort(std::string_view text) {
    if (text == "none") {
        return OpenAiReasoningEffort::kNone;
    }
    if (text == "minimal") {
        return OpenAiReasoningEffort::kMinimal;
    }
    if (text == "low") {
        return OpenAiReasoningEffort::kLow;
    }
    if (text == "medium") {
        return OpenAiReasoningEffort::kMedium;
    }
    if (text == "high") {
        return OpenAiReasoningEffort::kHigh;
    }
    if (text == "xhigh") {
        return OpenAiReasoningEffort::kXHigh;
    }
    return std::nullopt;
}

} // namespace isla::server::ai_gateway
