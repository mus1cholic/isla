#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/memory_types.hpp"

namespace isla::server::memory {

struct MidTermMemoryInit {
    std::string session_id;
    MemoryStorePtr store;
};

// DB-backed owner for the mid-term episode list. The canonical state lives in
// the configured MemoryStore; callers materialize vectors only as read results.
class MidTermMemory {
  public:
    [[nodiscard]] static absl::StatusOr<MidTermMemory> Create(const MidTermMemoryInit& init);

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    [[nodiscard]] absl::Status StoreEpisode(std::int64_t source_conversation_item_index,
                                            const Episode& episode);
    [[nodiscard]] absl::StatusOr<std::vector<Episode>> ListEpisodes() const;
    [[nodiscard]] absl::StatusOr<std::optional<Episode>>
    FindEpisode(std::string_view episode_id) const;
    [[nodiscard]] absl::StatusOr<bool> HasExpandableDetail(std::string_view episode_id) const;
    [[nodiscard]] absl::StatusOr<std::string>
    GetExpandableDetail(std::string_view episode_id) const;

  private:
    MidTermMemory(std::string session_id, MemoryStorePtr store);

    [[nodiscard]] absl::StatusOr<Episode> GetRequiredEpisode(std::string_view episode_id) const;

    std::string session_id_;
    MemoryStorePtr store_;
};

} // namespace isla::server::memory
