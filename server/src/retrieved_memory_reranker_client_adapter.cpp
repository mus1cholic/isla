#include "isla/server/retrieved_memory_reranker_client_adapter.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

namespace isla::server {
namespace {

absl::Status InvalidArgument(std::string message) {
    return absl::InvalidArgumentError(std::move(message));
}

class ClientBackedRetrievedMemoryReranker final
    : public isla::server::memory::RetrievedMemoryReranker {
  public:
    ClientBackedRetrievedMemoryReranker(std::shared_ptr<const RerankerClient> reranker_client,
                                        std::string model)
        : reranker_client_(std::move(reranker_client)), model_(std::move(model)) {}

    [[nodiscard]] absl::StatusOr<std::vector<double>>
    Score(std::string_view query,
          const std::vector<isla::server::memory::RetrievedMemoryCandidate>& candidates) override {
        std::vector<std::string> candidate_texts;
        candidate_texts.reserve(candidates.size());
        for (const isla::server::memory::RetrievedMemoryCandidate& candidate : candidates) {
            candidate_texts.push_back(candidate.candidate_text);
        }
        return reranker_client_->Rerank(RerankRequest{
            .model = model_,
            .query = std::string(query),
            .candidates = std::move(candidate_texts),
            .telemetry_context = nullptr,
        });
    }

  private:
    std::shared_ptr<const RerankerClient> reranker_client_;
    std::string model_;
};

} // namespace

absl::StatusOr<isla::server::memory::RetrievedMemoryRerankerPtr>
CreateClientBackedRetrievedMemoryReranker(std::shared_ptr<const RerankerClient> reranker_client,
                                          std::string model) {
    if (reranker_client == nullptr) {
        return InvalidArgument("retrieved-memory reranker adapter requires a reranker_client");
    }
    if (model.empty()) {
        return InvalidArgument("retrieved-memory reranker adapter requires a model");
    }
    return isla::server::memory::RetrievedMemoryRerankerPtr(
        std::make_shared<ClientBackedRetrievedMemoryReranker>(std::move(reranker_client),
                                                              std::move(model)));
}

} // namespace isla::server
