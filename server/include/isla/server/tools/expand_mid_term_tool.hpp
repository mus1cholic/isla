#pragma once

#include <string_view>

#include "isla/server/tools/tool.hpp"

namespace isla::server::tools {

// Public read-only tool that exposes full Tier 1 detail for one expandable mid-term episode.
//
// Intended behavior once wired:
// - the main LLM sees a `[expandable]` mid-term summary
// - it calls `expand_mid_term` with an `episode_id`
// - the server resolves that episode inside the current session and returns the exact Tier 1 text
//
// This scaffolding slice intentionally stops short of live memory integration. The tool still
// publishes its contract and validates arguments, but returns a structured placeholder error
// result until the execution loop and session-bound reader are connected.
class ExpandMidTermTool final : public Tool {
  public:
    static constexpr std::string_view kName = "expand_mid_term";

    [[nodiscard]] ToolDefinition Definition() const override;

    [[nodiscard]] absl::StatusOr<ToolResult> Execute(const ToolExecutionContext& context,
                                                     const ToolCall& call) const override;
};

} // namespace isla::server::tools
