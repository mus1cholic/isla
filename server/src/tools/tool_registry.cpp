#include "isla/server/tools/tool_registry.hpp"

#include <utility>

#include "absl/status/status.h"

namespace isla::server::tools {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

ToolRegistry::ToolRegistry(std::vector<std::shared_ptr<const Tool>> tools,
                           absl::flat_hash_map<std::string, std::size_t> indices_by_name)
    : tools_(std::move(tools)), indices_by_name_(std::move(indices_by_name)) {}

absl::StatusOr<ToolRegistry> ToolRegistry::Create(std::vector<std::shared_ptr<const Tool>> tools) {
    absl::flat_hash_map<std::string, std::size_t> indices_by_name;
    indices_by_name.reserve(tools.size());

    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (tools[i] == nullptr) {
            return invalid_argument("tool registry cannot register a null tool");
        }
        const ToolDefinition definition = tools[i]->Definition();
        if (definition.name.empty()) {
            return invalid_argument("tool registry cannot register a tool with an empty name");
        }
        if (!indices_by_name.insert({ definition.name, i }).second) {
            return invalid_argument("tool registry cannot register duplicate tool names");
        }
    }

    return ToolRegistry(std::move(tools), std::move(indices_by_name));
}

std::vector<ToolDefinition> ToolRegistry::ListDefinitions() const {
    std::vector<ToolDefinition> definitions;
    definitions.reserve(tools_.size());
    for (const std::shared_ptr<const Tool>& tool : tools_) {
        definitions.push_back(tool->Definition());
    }
    return definitions;
}

bool ToolRegistry::Contains(std::string_view name) const {
    return indices_by_name_.contains(std::string(name));
}

absl::StatusOr<ToolResult> ToolRegistry::Execute(const ToolExecutionContext& context,
                                                 const ToolCall& call) const {
    const auto it = indices_by_name_.find(call.name);
    if (it == indices_by_name_.end()) {
        return absl::NotFoundError("tool registry could not find the requested tool");
    }

    absl::StatusOr<ToolResult> result = tools_[it->second]->Execute(context, call);
    if (!result.ok()) {
        return result.status();
    }
    if (result->call_id.empty()) {
        result->call_id = call.call_id;
    }
    if (result->tool_name.empty()) {
        result->tool_name = call.name;
    }
    return result;
}

} // namespace isla::server::tools
