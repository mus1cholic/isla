#include "isla/server/memory/system_prompt.hpp"

#include <string>

#include "absl/status/statusor.h"
#include "isla/server/memory/prompt_loader.hpp"
#include "isla/server/memory/working_memory_utils.hpp"

namespace isla::server::memory {

SystemPrompt::SystemPrompt(SystemPromptState state) : state_(std::move(state)) {}

absl::StatusOr<SystemPrompt> SystemPrompt::Create(std::string_view configured_prompt) {
    absl::StatusOr<SystemPromptState> state = CreateSystemPromptState(configured_prompt);
    if (!state.ok()) {
        return state.status();
    }
    return SystemPrompt(std::move(*state));
}

absl::StatusOr<std::string> SystemPrompt::Render() const {
    return RenderSystemPrompt(state_);
}

absl::StatusOr<SystemPromptState> CreateSystemPromptState(std::string_view configured_prompt) {
    absl::StatusOr<std::string> base_instructions = ResolveSystemPrompt(configured_prompt);
    if (!base_instructions.ok()) {
        return base_instructions.status();
    }
    return SystemPromptState{
        .base_instructions = std::move(*base_instructions),
        .persistent_memory_cache = {},
    };
}

absl::StatusOr<std::string> RenderSystemPrompt(const SystemPromptState& system_prompt) {
    std::string output;
    output.append(system_prompt.base_instructions);
    if (!output.empty() && output.back() != '\n') {
        output.push_back('\n');
    }

    AppendPersistentMemoryCacheSection(output, system_prompt.persistent_memory_cache);
    return output;
}

} // namespace isla::server::memory
