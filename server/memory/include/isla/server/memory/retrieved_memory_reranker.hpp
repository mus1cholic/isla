#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace isla::server::memory {

enum class RetrievedMemoryCandidateKind {
    KgFact,
    Episode,
};

struct RetrievedMemoryCandidate {
    RetrievedMemoryCandidateKind kind = RetrievedMemoryCandidateKind::KgFact;
    std::string source_id;
    std::string candidate_text;
};

class RetrievedMemoryReranker {
  public:
    virtual ~RetrievedMemoryReranker() = default;

    [[nodiscard]] virtual absl::StatusOr<std::vector<double>>
    Score(std::string_view query, const std::vector<RetrievedMemoryCandidate>& candidates) = 0;
};

using RetrievedMemoryRerankerPtr = std::shared_ptr<RetrievedMemoryReranker>;

} // namespace isla::server::memory
