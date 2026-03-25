#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "isla/server/evals/eval_types.hpp"

namespace isla::server::evals {

struct LiveEvalRunnerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string path = "/";
    std::chrono::milliseconds operation_timeout{ std::chrono::seconds(10) };
    std::chrono::milliseconds turn_completion_timeout{ std::chrono::seconds(60) };
};

class LiveEvalRunner final {
  public:
    explicit LiveEvalRunner(LiveEvalRunnerConfig config = {});

    [[nodiscard]] absl::StatusOr<EvalArtifacts> RunCase(const EvalCase& eval_case) const;

  private:
    LiveEvalRunnerConfig config_;
};

} // namespace isla::server::evals
