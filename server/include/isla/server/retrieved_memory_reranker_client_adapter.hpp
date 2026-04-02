#pragma once

#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/memory/retrieved_memory_reranker.hpp"
#include "isla/server/reranker_client.hpp"

namespace isla::server {

[[nodiscard]] absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr>
CreateClientBackedRetrievedMemoryReranker(std::shared_ptr<const RerankerClient> reranker_client,
                                          std::string model);

} // namespace isla::server
