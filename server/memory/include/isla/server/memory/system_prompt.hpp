#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

class SystemPrompt {
  public:
    explicit SystemPrompt(SystemPromptState state);

    [[nodiscard]] static absl::StatusOr<SystemPrompt> Create(std::string_view configured_prompt);

    [[nodiscard]] const SystemPromptState& snapshot() const {
        return state_;
    }

    [[nodiscard]] PersistentMemoryCache& mutable_persistent_memory_cache() {
        return state_.persistent_memory_cache;
    }

    [[nodiscard]] const PersistentMemoryCache& persistent_memory_cache() const {
        return state_.persistent_memory_cache;
    }

    [[nodiscard]] absl::StatusOr<std::string> Render() const;

  private:
    SystemPromptState state_;
};

[[nodiscard]] absl::StatusOr<SystemPromptState>
CreateSystemPromptState(std::string_view configured_prompt);

[[nodiscard]] absl::StatusOr<std::string> RenderSystemPrompt(const SystemPromptState& system_prompt);

} // namespace isla::server::memory
