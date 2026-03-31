#pragma once

#include <gmock/gmock.h>

#include "isla/server/memory/mid_term_flush_decider.hpp"

namespace isla::server::memory::test {

class MockMidTermFlushDecider : public MidTermFlushDecider {
  public:
    MOCK_METHOD((absl::StatusOr<MidTermFlushDecision>), Decide, (const Conversation& conversation),
                (override));
};

} // namespace isla::server::memory::test
