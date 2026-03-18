#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::ai_gateway {
class OpenAiResponsesClient;
} // namespace isla::server::ai_gateway

namespace isla::server::memory {

struct MidTermFlushDecision {
    bool should_flush = false;
    std::optional<std::size_t> conversation_item_index;
    std::optional<std::size_t> split_at_message_index;
};

class MidTermFlushDecider {
  public:
    virtual ~MidTermFlushDecider() = default;

    // Chooses whether a conversation item should flush to mid-term memory. When `should_flush` is
    // true, `conversation_item_index` must be set; `split_at_message_index` is optional and, when
    // present, refers to the first user message that should remain in working memory.
    [[nodiscard]] virtual absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) = 0;
};

using MidTermFlushDeciderPtr = std::shared_ptr<MidTermFlushDecider>;

// TODO: Introduce an LlmClient abstraction to decouple the flush decider (and
// future LLM-based components) from the OpenAI-specific transport. The concrete
// OpenAiResponsesClient would become one implementation of that interface.
// Creates the LLM-backed flush decider. The returned implementation validates the model's JSON
// response against the live conversation before handing the decision back to the orchestrator.
[[nodiscard]] absl::StatusOr<MidTermFlushDeciderPtr> CreateLlmMidTermFlushDecider(
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> responses_client,
    std::string model);

} // namespace isla::server::memory
