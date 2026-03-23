#pragma once

#include <optional>

#include "isla/server/llm_client.hpp"
#include "isla/server/openai_reasoning_effort.hpp"

namespace isla::server::ai_gateway {

[[nodiscard]] constexpr std::optional<isla::server::LlmReasoningEffort>
TryOpenAiReasoningEffortToLlmReasoningEffort(OpenAiReasoningEffort effort) {
    switch (effort) {
    case OpenAiReasoningEffort::kNone:
        return isla::server::LlmReasoningEffort::kNone;
    case OpenAiReasoningEffort::kMinimal:
        return isla::server::LlmReasoningEffort::kMinimal;
    case OpenAiReasoningEffort::kLow:
        return isla::server::LlmReasoningEffort::kLow;
    case OpenAiReasoningEffort::kMedium:
        return isla::server::LlmReasoningEffort::kMedium;
    case OpenAiReasoningEffort::kHigh:
        return isla::server::LlmReasoningEffort::kHigh;
    case OpenAiReasoningEffort::kXHigh:
        return isla::server::LlmReasoningEffort::kXHigh;
    }
    return std::nullopt;
}

} // namespace isla::server::ai_gateway
