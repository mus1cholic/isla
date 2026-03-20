#pragma once

#include "absl/status/statusor.h"
#include "isla/server/tools/tool_context.hpp"
#include "isla/server/tools/tool_types.hpp"

namespace isla::server::tools {

// Provider-neutral tool interface exposed by the gateway.
//
// This sits above any specific LLM provider and below any specific application tool. The goal is
// to let the rest of the gateway reason in terms of "tool definitions", "tool calls", and
// "tool results" without committing the server to OpenAI-specific request/response types.
class Tool {
  public:
    virtual ~Tool() = default;

    // Returns the public contract advertised to the model. Definitions should be stable and
    // deterministic because provider adapters may cache or diff them between turns.
    [[nodiscard]] virtual ToolDefinition Definition() const = 0;

    // Executes one tool call.
    //
    // Implementations should prefer returning `ToolResult{ .is_error = true, ... }` for expected
    // tool-level failures that the model can recover from, such as bad arguments or missing
    // episode ids. Non-OK statuses are reserved for framework-level failures such as impossible
    // dispatch state or invariant violations.
    [[nodiscard]] virtual absl::StatusOr<ToolResult>
    Execute(const ToolExecutionContext& context, const ToolCall& call) const = 0;
};

} // namespace isla::server::tools
