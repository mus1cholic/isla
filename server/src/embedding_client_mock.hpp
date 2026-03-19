#pragma once

#include <gmock/gmock.h>

#include "isla/server/embedding_client.hpp"

namespace isla::server::test {

class MockEmbeddingClient : public EmbeddingClient {
  public:
    MOCK_METHOD(absl::Status, Validate, (), (const, override));
    MOCK_METHOD(absl::Status, WarmUp, (), (const, override));
    MOCK_METHOD((absl::StatusOr<isla::server::memory::Embedding>), Embed,
                (const EmbeddingRequest& request), (const, override));
};

} // namespace isla::server::test
