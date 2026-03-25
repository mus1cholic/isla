#pragma once

#include <string>
#include <string_view>

#include "isla/server/openai_reasoning_effort.hpp"

namespace isla::server::ai_gateway {

// Runtime-configurable default models. These values are the server's fallback
// choices when no override is provided via:
//   --main-llm-model / AI_GATEWAY_MAIN_LLM_MODEL
//   --mid-term-flush-decider-model / AI_GATEWAY_MID_TERM_FLUSH_DECIDER_MODEL
//   --mid-term-compactor-model / AI_GATEWAY_MID_TERM_COMPACTOR_MODEL
//   --mid-term-embedding-model / AI_GATEWAY_MID_TERM_EMBEDDING_MODEL
inline constexpr std::string_view kDefaultMainLlmModel = "gpt-5.3-chat-latest";
inline constexpr std::string_view kDefaultMidTermFlushDeciderModel = "gpt-5.4-mini";
inline constexpr std::string_view kDefaultMidTermCompactorModel = "gpt-5.4-mini";
inline constexpr std::string_view kDefaultMidTermEmbeddingModel = "gemini-embedding-2-preview";
inline constexpr OpenAiReasoningEffort kDefaultReasoningEffort = OpenAiReasoningEffort::kMedium;

struct GatewayLlmRuntimeConfig {
    std::string main_model;
    std::string mid_term_flush_decider_model;
    std::string mid_term_compactor_model;
    std::string mid_term_embedding_model;
    OpenAiReasoningEffort reasoning_effort = kDefaultReasoningEffort;
};

} // namespace isla::server::ai_gateway
