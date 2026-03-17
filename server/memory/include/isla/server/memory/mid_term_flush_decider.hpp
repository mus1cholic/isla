#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "absl/status/statusor.h"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct MidTermFlushDecision {
    bool should_flush = false;
    std::optional<std::size_t> conversation_item_index;
};

class MidTermFlushDecider {
  public:
    virtual ~MidTermFlushDecider() = default;

    [[nodiscard]] virtual absl::StatusOr<MidTermFlushDecision>
    Decide(const Conversation& conversation) = 0;
};

using MidTermFlushDeciderPtr = std::shared_ptr<MidTermFlushDecider>;

} // namespace isla::server::memory
