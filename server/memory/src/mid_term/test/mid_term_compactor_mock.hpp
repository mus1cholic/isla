#pragma once

#include <gmock/gmock.h>

#include "isla/server/memory/mid_term_compactor.hpp"

namespace isla::server::memory::test {

class MockMidTermCompactor : public MidTermCompactor {
  public:
    MOCK_METHOD((absl::StatusOr<CompactedMidTermEpisode>), Compact,
                (const MidTermCompactionRequest& request), (override));
};

} // namespace isla::server::memory::test
