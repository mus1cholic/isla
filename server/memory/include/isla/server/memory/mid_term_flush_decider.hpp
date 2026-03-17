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

    [[nodiscard]] virtual absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) = 0;
};

using MidTermFlushDeciderPtr = std::shared_ptr<MidTermFlushDecider>;

// TODO: Future PR should make the decider call async with the compactor chained
// as a dependent/blocking step, so the LLM round-trip does not block the
// orchestrator thread.
//
// TODO: Introduce an LlmClient abstraction to decouple the flush decider (and
// future LLM-based components) from the OpenAI-specific transport. The concrete
// OpenAiResponsesClient would become one implementation of that interface.
[[nodiscard]] MidTermFlushDeciderPtr CreateLlmMidTermFlushDecider(
    std::shared_ptr<const isla::server::ai_gateway::OpenAiResponsesClient> responses_client,
    std::string model);

} // namespace isla::server::memory
