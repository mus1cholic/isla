#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server::tools {

// Session-scoped read surface that tools may use to resolve memory-backed requests.
//
// This indirection is deliberate. Tools should not know how to reach directly into storage,
// responder internals, or provider-specific execution state. Instead, the gateway can supply a
// session-bound reader implementation that exposes only the narrow, read-only operations the tool
// contract needs.
class ToolSessionReader {
  public:
    virtual ~ToolSessionReader() = default;

    // Returns the full Tier 1 detail for one expandable mid-term episode in the current session.
    // Implementations should return NotFound when the episode does not exist and
    // FailedPrecondition when the episode exists but is not expandable.
    [[nodiscard]] virtual absl::StatusOr<std::string>
    ExpandMidTermEpisode(std::string_view episode_id) const = 0;
};

// Shared execution context for one tool invocation.
//
// The context is intentionally lightweight and request-scoped:
// - `session_id` identifies which session the tool is operating against.
// - `telemetry_context` lets later wiring attribute tool latency and outcomes to the current turn.
// - `session_reader` is optional in this scaffolding slice because tool dispatch is not yet wired
//   into live session memory.
struct ToolExecutionContext {
    std::string session_id;
    std::shared_ptr<const isla::server::ai_gateway::TurnTelemetryContext> telemetry_context;
    std::shared_ptr<const ToolSessionReader> session_reader;
};

} // namespace isla::server::tools
