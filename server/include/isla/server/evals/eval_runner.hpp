#pragma once

#include <chrono>
#include <memory>

#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/evals/eval_types.hpp"

namespace isla::server::evals {

struct EvalRunnerConfig {
    isla::server::ai_gateway::GatewayStubResponderConfig responder_config;
    std::shared_ptr<const isla::server::ai_gateway::TelemetrySink> telemetry_sink =
        isla::server::ai_gateway::CreateNoOpTelemetrySink();
    // Bounds working-memory snapshot settling only. Phase 3.3 moved live turn completion onto
    // the responder's direct blocking turn seam, so this no longer governs model execution time.
    std::chrono::milliseconds session_settle_timeout{ std::chrono::seconds(2) };
};

// App-boundary eval runner for benchmark cases.
//
// This intentionally enters through the same application-owned responder path used in production
// while replacing the live transport with an in-process recording session. That keeps memory
// shaping, planning, execution, and reply handling on the real path without paying WebSocket
// overhead for every benchmark case.
class EvalRunner final {
  public:
    explicit EvalRunner(EvalRunnerConfig config = {});

    [[nodiscard]] absl::StatusOr<EvalArtifacts> RunCase(const EvalCase& eval_case) const;

  private:
    EvalRunnerConfig config_;
};

// Normalizes one benchmark-owned conversation timeline into the app-boundary runner's current
// case shape. The designated evaluated turn must currently be the final turn in the supplied
// timeline because the runner captures artifacts around exactly one live turn.
[[nodiscard]] absl::StatusOr<EvalCase>
BuildEvalCaseFromBenchmarkTimeline(const EvalBenchmarkTimelineCase& timeline_case);

} // namespace isla::server::evals
