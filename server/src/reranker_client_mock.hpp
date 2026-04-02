#pragma once

#include <gmock/gmock.h>

#include "isla/server/reranker_client.hpp"

namespace isla::server::test {

class MockRerankerClient : public RerankerClient {
  public:
    MOCK_METHOD(absl::Status, Validate, (), (const, override));
    MOCK_METHOD(absl::Status, WarmUp, (), (const, override));
    MOCK_METHOD((absl::StatusOr<std::vector<double>>), Rerank, (const RerankRequest& request),
                (const, override));
};

} // namespace isla::server::test
