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
    // Creates a mid-term memory view backed by the configured store. The store is required because
    // this type treats persistence as the canonical source of truth.
    [[nodiscard]] static absl::StatusOr<MidTermMemory> Create(const MidTermMemoryInit& init);

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    // Persists a compacted episode for this session, preserving the source conversation item index
    // so the stored episode can still be traced back to its working-memory origin.
    [[nodiscard]] absl::Status StoreEpisode(std::int64_t source_conversation_item_index,
                                            const Episode& episode);

    // Lists all persisted mid-term episodes for the session.
    [[nodiscard]] absl::StatusOr<std::vector<Episode>> ListEpisodes() const;

    // Looks up one mid-term episode by id and returns nullopt when the episode is simply absent.
    [[nodiscard]] absl::StatusOr<std::optional<Episode>>
    FindEpisode(std::string_view episode_id) const;

    // Reports whether the episode exists and carries Tier 1 detail that meets the expandability
    // rules, rather than merely checking tier1_detail for non-null.
    [[nodiscard]] absl::StatusOr<bool> HasExpandableDetail(std::string_view episode_id) const;

    // Returns Tier 1 detail for episodes that are marked expandable. Missing episodes return
    // NotFound, while non-expandable episodes return FailedPrecondition.
    [[nodiscard]] absl::StatusOr<std::string>
    GetExpandableDetail(std::string_view episode_id) const;

  private:
    MidTermMemory(std::string session_id, MemoryStorePtr store);

    [[nodiscard]] absl::StatusOr<Episode> GetRequiredEpisode(std::string_view episode_id) const;

    std::string session_id_;
    MemoryStorePtr store_;
};

} // namespace isla::server::memory
