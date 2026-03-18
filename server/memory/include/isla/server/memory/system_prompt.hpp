#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

class SystemPrompt {
  public:
    explicit SystemPrompt(SystemPromptState state);

    // Resolves the configured prompt text, falling back to the embedded default when empty, and
    // initializes an empty persistent-memory cache around that base prompt.
    [[nodiscard]] static absl::StatusOr<SystemPrompt> Create(std::string_view configured_prompt);

    [[nodiscard]] const SystemPromptState& snapshot() const {
        return state_;
    }

    PersistentMemoryCache& mutable_persistent_memory_cache() {
        return state_.persistent_memory_cache;
    }

    [[nodiscard]] const PersistentMemoryCache& persistent_memory_cache() const {
        return state_.persistent_memory_cache;
    }

    // Renders the final prompt text by concatenating base instructions and the current
    // persistent-memory cache section.
    [[nodiscard]] absl::StatusOr<std::string> Render() const;

  private:
    SystemPromptState state_;
};

// Builds a serializable system-prompt state object from either configured text or the embedded
// default prompt asset.
[[nodiscard]] absl::StatusOr<SystemPromptState>
CreateSystemPromptState(std::string_view configured_prompt);

// Renders base instructions plus the persistent-memory cache section, ensuring a separating
// trailing newline between them when needed.
[[nodiscard]] absl::StatusOr<std::string>
RenderSystemPrompt(const SystemPromptState& system_prompt);

} // namespace isla::server::memory
