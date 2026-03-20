#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "isla/server/tools/tool.hpp"

namespace isla::server::tools {

// Stable registry of all public tools available to a model-facing execution loop.
//
// The registry owns no provider logic. Its job is simply:
// - validate and index tool definitions
// - expose the public list of tool contracts in deterministic order
// - route one tool call to the matching implementation
class ToolRegistry {
  public:
    // Builds a registry and rejects duplicate or invalid tool names up front so later dispatch
    // can stay simple and deterministic.
    [[nodiscard]] static absl::StatusOr<ToolRegistry>
    Create(std::vector<std::shared_ptr<const Tool>> tools);

    // Returns all public tool definitions in registration order. This ordering is expected to be
    // stable so provider adapters can pass through a consistent list turn to turn.
    [[nodiscard]] std::vector<ToolDefinition> ListDefinitions() const;

    // Reports whether a tool with the given public name is registered.
    [[nodiscard]] bool Contains(std::string_view name) const;

    // Dispatches one call by name.
    //
    // The registry normalizes missing `call_id` and `tool_name` fields in returned ToolResult
    // values so tool implementations can focus on their own logic rather than bookkeeping.
    [[nodiscard]] absl::StatusOr<ToolResult> Execute(const ToolExecutionContext& context,
                                                     const ToolCall& call) const;

  private:
    ToolRegistry(std::vector<std::shared_ptr<const Tool>> tools,
                 absl::flat_hash_map<std::string, std::size_t> indices_by_name);

    std::vector<std::shared_ptr<const Tool>> tools_;
    absl::flat_hash_map<std::string, std::size_t> indices_by_name_;
};

} // namespace isla::server::tools
