#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct MidTermFlushBoundary {
    std::size_t split_before_message_index = 0;
    std::optional<std::string> reasoning;
};

struct MidTermFlushDecision {
    std::vector<MidTermFlushBoundary> boundaries;
    bool tail_complete = false;
    // Free-form text from the model explaining why it chose these boundaries (or no boundaries).
    // Intended for diagnostics and logging only, not for machine parsing.
    std::optional<std::string> reasoning;
};

class MidTermFlushDecider {
  public:
    virtual ~MidTermFlushDecider() = default;

    // Returns the ordered topic boundaries for the final live ongoing episode plus whether the
    // remaining tail has also concluded. An empty boundary list with tail_complete=false means
    // the conversation should remain live with no flush.
    [[nodiscard]] virtual absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) = 0;
};

using MidTermFlushDeciderPtr = std::shared_ptr<MidTermFlushDecider>;

// Creates the LLM-backed flush decider. The returned implementation validates the model's JSON
// response against the live conversation before handing the decision back to the orchestrator.
[[nodiscard]] absl::StatusOr<MidTermFlushDeciderPtr> CreateLlmMidTermFlushDecider(
    std::shared_ptr<const isla::server::LlmClient> llm_client, std::string model,
    isla::server::LlmReasoningEffort reasoning_effort = isla::server::LlmReasoningEffort::kNone);

} // namespace isla::server::memory
