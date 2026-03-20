#pragma once

#include <string>

namespace isla::server::tools {

// Provider-neutral description of one callable tool exposed to the LLM.
//
// The schema is stored as a serialized JSON Schema string on purpose. This keeps the public API
// independent from any single provider's native tool descriptor types while still preserving the
// exact contract that adapters will later forward to providers such as OpenAI Responses.
struct ToolDefinition {
    // Stable tool identifier used both in provider payloads and server-side dispatch.
    std::string name;

    // Natural-language guidance for the model about when the tool should be used.
    std::string description;

    // Serialized JSON Schema describing the accepted argument object.
    std::string input_json_schema;

    // Whether the tool promises not to mutate durable state. This is informational for now, but
    // it is expected to matter later for planning, auditing, and provider-specific tool policies.
    bool read_only = true;
};

// One model-requested tool invocation.
//
// `call_id` is the provider-facing handle that ties the eventual tool result back to the specific
// tool call in the same turn. The server should preserve it exactly when echoing tool results.
struct ToolCall {
    std::string call_id;
    std::string name;
    std::string arguments_json;
};

// Result returned to the model after a tool call finishes.
//
// The distinction between `is_error=true` and a non-OK status from the execution framework is
// important:
// - `is_error=true` means the tool ran to completion and produced a model-visible error result
//   that should usually be fed back into the turn.
// - a non-OK `absl::Status` means the server itself failed to route or execute the tool call.
struct ToolResult {
    std::string call_id;
    std::string tool_name;
    std::string output_text;
    bool is_error = false;
};

} // namespace isla::server::tools
