# AI Gateway Evaluation Phased Plan

Last updated: 2026-03-22

## Purpose

This document freezes the initial evaluation roadmap for the `isla` AI gateway and memory-backed
turn pipeline.

It is the authoritative phased plan for:

- benchmark-driven evaluation of the current end-to-end turn pipeline
- time-aware evaluation required by memory benchmarks such as LOCOMO and LongMemEval
- multi-rater evaluation of memory behavior and final answer quality
- artifact capture, reporting, and regression gating for future model and memory changes

Later evaluation work should extend this document instead of redefining evaluation contracts in
chat, code comments, or one-off scripts.

## Relationship To Existing Docs

This plan builds on the current architecture and memory references rather than replacing them:

- `docs/ai/ai_gateway_v1_design.md` remains the authoritative v1 gateway ownership and turn-flow
  baseline.
- `docs/ai/ai-gateway-phased-plan.md` remains the authoritative implementation history and gateway
  rollout plan.
- `docs/ai/ai-gateway-telemetry-phased-plan.md` remains the authoritative latency and telemetry
  baseline.
- `docs/memory/memory-planning.md` remains the authoritative working/mid-term/long-term memory
  architecture reference.

Evaluation work MUST preserve the boundaries already established in those documents.

## Context

`isla` currently has:

- a runnable WebSocket-based gateway server
- a transport-facing session handler
- an application-owned `GatewayStubResponder`
- a planner/executor boundary
- a live OpenAI Responses provider path behind the executor seam
- a working-memory and mid-term-memory path integrated into the responder
- provider-neutral tool scaffolding and a declared `expand_mid_term` tool contract

The current implementation also has explicit evaluation-relevant limitations:

- a small app-boundary eval runner now exists, but there are still no benchmark adapters,
  autoraters, or standalone eval CLI surfaces yet
- the live gateway path records memory event times with wall-clock `NowTimestamp()` in the
  responder path instead of allowing benchmark-supplied event time
- mid-term flush stub timestamps are also generated from wall-clock time inside the orchestrator
- the current prompt does not inject an explicit benchmark "current time" for temporally-scoped
  evaluation questions
- the current runner captures prompt artifacts, emitted events, and structured mid-term snapshots,
  but it does not yet capture expansion traces, flush/compaction traces, or benchmark-supplied
  time metadata

> [!NOTE]
> **Current status (2026-03-20):**
> - Phase 0 is implemented.
> - Phase 1 is partially implemented as an initial slice.
> - Phase 2 is implemented for the evaluation pipeline.
> - Phase 3 is implemented for the local benchmark slice.
> - Phase 3.1 is implemented as replay-fidelity follow-up work discovered during Phase 3 rollout.
> - Phase 3.2 is implemented to switch benchmark execution from fake-provider main-turn replies to
>   the real main LLM path.
> - Phase 3.3 is implemented to add a backend-owned direct turn-execution seam that serving and
>   evaluation can share.
> - Phase 3.4 is implemented to replace timeout-based post-turn memory settling with explicit
>   blocking awaits through a backend-owned memory-settled seam.
> - The current implemented Phase-1 slice now provides:
>   - a small benchmark-first eval core in:
>     - `server/include/isla/server/evals/eval_types.hpp`
>     - `server/include/isla/server/evals/eval_runner.hpp`
>     - `server/src/evals/eval_runner.cpp`
>   - a canonical `gateway_app_boundary` runner that executes through
>     `GatewayStubResponder` with an in-process recording session instead of the WebSocket layer
>   - captured prompt artifacts for the evaluated turn:
>     - rendered system prompt
>     - rendered working-memory context
>     - rendered full prompt
>   - structured pre-turn and post-turn mid-term snapshots using the live responder-owned session
>     memory
>   - emitted turn event capture for the evaluated turn
>   - focused regression coverage in:
>     - `server/src/evals/eval_runner_test.cpp`
> - The current implemented slice does not yet provide:
>   - benchmark adapters such as LOCOMO or LongMemEval
>   - standalone eval binaries or JSON-driven eval input
>   - autoraters
>   - full structured trace capture for expansion, flush, or compaction decisions
> - The current implemented Phase-2 slice now provides:
>   - optional benchmark-supplied session start, user-message, and assistant-message timestamps
>     through the app-boundary eval runner
>   - chronology-aligned mid-term stub timestamps derived from conversation message time instead of
>     wall-clock time
>   - a minimal benchmark-timeline adapter utility that normalizes benchmark-owned turn timelines
>     into the canonical `EvalCase` shape
>   - setup-turn replay directly into session memory while preserving the evaluated turn as the
>     live executed turn through the real responder path
>   - a normalized benchmark timeline artifact that records canonical replayed session history:
>     session start, user/assistant chronology, and evaluation reference time
>   - focused regression coverage proving replay-time propagation and persisted session/message
>     timestamps through the real responder path
> - The current implemented Phase-3 slice now provides:
>   - a runnable `isla_custom_memory` benchmark surface in:
>     - `server/include/isla/server/evals/isla_custom_memory_benchmark.hpp`
>     - `server/src/evals/isla_custom_memory_benchmark.cpp`
>     - `server/src/evals/isla_custom_memory_main.cpp`
>     - `server/src/evals/isla_custom_memory_benchmark_test.cpp`
>   - five hand-authored local benchmark cases covering:
>     - direct fact recall
>     - recency and contradiction handling
>     - mid-term episode visibility
>     - expandable exact-detail retrieval through the live `expand_mid_term` tool loop
>     - timestamp-aware replay diagnostics with explicit benchmark clocks
>   - persisted per-case artifact JSON plus a benchmark-level `report.json`
>   - deterministic fake-provider execution only for the model/provider seam, while still reading
>     the real rendered prompt and working-memory context from the app-boundary runner
>   - benchmark setup replay without synthetic runtime-generated setup replies
>   - replayed assistant history only for multi-turn setup state, not for the live evaluated turn
> - The current implemented Phase-3.1 slice now provides:
>   - a responder-owned `GatewaySessionClock` seam shared by eval replay and future serving-path
>     reference-time work instead of separate timestamp override callbacks
>   - replayed-session-history artifacts as the canonical structured replay output for eval runs
>   - eval-specific persistence identity through benchmark-derived `memory_user_id` values such as
>     `eval_isla_custom_memory`
>   - evaluation reference time carried explicitly as replayed session chronology in those artifacts
> - The current implemented Phase-3.2 slice now provides:
>   - real OpenAI Responses execution for the evaluated benchmark turn
>   - shared gateway startup-config parsing for the standalone benchmark binary so it can reuse the
>     serving path's OpenAI and model configuration
>   - live-only benchmark execution for the evaluated turn, with benchmark-owned stubbing retained
>     only for mid-term decider/compactor responses that still stabilize artifact expectations
> - Separate from evaluation infrastructure, the real product path still does not yet provide:
>   - product-visible support for a notion of current/reference time inside the real prompt path
>   - prompt decoration or timing rules specific to individual external benchmark formats

Those limitations make time-aware evaluation a first-order requirement rather than a later polish
task.

## Goals

The first evaluation roadmap should answer these questions reliably:

- Does the current memory pipeline answer benchmark questions using the right remembered facts?
- Does the final assistant reply stay faithful to the available memory context?
- When a memory benchmark includes long gaps or relative-time reasoning, can Isla be evaluated
  against the benchmark chronology instead of the machine's wall clock?
- Can one benchmark run multiple autoraters over the same captured turn artifacts?
- Can evaluations be reported and gated by benchmark without forcing the repository into a more
  abstract grouping model too early?
- Can the evaluation framework grow later into tool-use and holistic system evaluation without
  invalidating the initial benchmark-first organization?

## Non-Goals

Phase 0 intentionally does not define:

- production-facing online evaluation or shadow traffic
- a benchmark-agnostic "family" taxonomy as a first-class architecture concept
- fully automated judge calibration against human labels in the first slice
- client-visible UI for browsing eval results
- tool-use evaluation as a blocking dependency for the first memory benchmark slice
- long-term-memory retrieval evaluation beyond the currently implemented memory surfaces

## Recommendation

Implement evaluations in four layers:

- a small generic eval core
- benchmark-specific adapters
- benchmark-configured autoraters
- benchmark-grouped reports

The repository should stay benchmark-first for now:

- benchmark identity is the primary organizational unit
- one benchmark may run multiple autoraters
- reports are grouped by benchmark, not by cross-benchmark evaluation family

The core SHOULD remain generic enough that later work can slice across benchmarks if needed, but
that capability SHOULD remain a consequence of the data model rather than a forced organizing
principle in the first implementation.

## Canonical Eval Injection Point

The evaluation framework MUST make the execution entrypoint explicit.

The recommended default injection point for benchmark execution is the application-owned gateway
boundary, not the raw WebSocket transport layer and not the bare planner/executor boundary.

In practice, this means benchmark evals SHOULD:

- enter through the same responder-facing application event surface used by production turn
  orchestration
- preserve real memory shaping, prompt rendering, planning, execution, and final reply handling
- use an in-process recording session or equivalent transport substitute instead of requiring a
  real WebSocket client for every benchmark case

Rationale:

- WebSocket-level evals are the heaviest and noisiest path and are not a good default for
  benchmark-scale runs.
- Planner/executor-only evals are too low-level for the current priorities because they skip the
  memory-backed responder path that we most want to evaluate first.
- The application boundary is the narrowest seam that still exercises the current end-to-end
  memory plus final-answer pipeline.

Recommended evaluation modes:

- `gateway_app_boundary`
  - canonical benchmark mode
  - enters through the application-owned responder boundary
  - captures memory shaping, execution, and reply behavior without network/protocol overhead
- `transport_e2e`
  - optional transport-contract mode
  - runs through `GatewayServer` and the WebSocket layer
  - reserved for smaller protocol and ingress smoke suites, not the default benchmark path
- `component`
  - optional focused mode
  - runs against lower-level seams such as planner, executor, or raters for targeted debugging
  - useful for diagnosis, but not the source of truth for benchmark results

Rules:

- benchmark reports for the initial memory and final-answer roadmap SHOULD default to
  `gateway_app_boundary`
- `transport_e2e` runs MAY exist for confidence in ingress and protocol behavior, but SHOULD stay
  small and separate from benchmark-scale runs
- `component` evals MAY exist for debugging and targeted regression isolation, but SHOULD NOT
  replace the canonical benchmark path

## Architecture Constraints

The evaluation contract MUST respect the current architecture:

- The desktop client talks only to the application-owned gateway.
- The WebSocket/session layer does not own provider I/O.
- The planner emits one execution plan in v1.
- The executor performs one OpenAI request in v1.
- The memory system remains a responder concern.
- Async emit completion means transport-executor acceptance or rejection, not remote socket flush.
- One accepted turn is in flight per session in v1.

The evaluation contract MUST also reflect the written memory design:

- Working memory is the immediate low-latency memory layer.
- Mid-term Tier 2 summaries are always present in context.
- `expand_mid_term` exists to recover exact Tier 1 detail when a summary is tagged
  `[expandable]`.
- Long-term retrieval remains a staged concern beyond the current implemented responder path.

## Evaluation Model

### Primary Unit

The primary unit of evaluation is one benchmark case executed through the Isla gateway turn path.

Each executed case produces:

- the case metadata
- the benchmark identity
- the benchmark-supplied conversation and time data
- the final assistant reply
- captured memory/rendering artifacts
- telemetry and execution metadata
- zero or more rater results

### Canonical Artifacts

The initial eval runner SHOULD capture at least:

- benchmark name and case id
- session id and turn id
- benchmark event timeline
- benchmark evaluation reference time
- rendered system prompt
- rendered working-memory context
- rendered full prompt
- prompt-visible mid-term episode rendering as seen by the model
- structured pre-turn mid-term memory snapshot
  - episode id
  - created_at
  - salience
  - tier2 summary
  - expandable eligibility
- structured post-turn mid-term memory snapshot
  - episode id
  - created_at
  - salience
  - tier2 summary
  - expandable eligibility
- mid-term expansion trace when applicable
  - which episodes were eligible for expansion
  - which episodes were expanded
  - whether expansion succeeded or failed
- mid-term flush or compaction trace when applicable
  - newly created episode ids
  - split flush behavior
  - relevant compaction outputs needed to explain later prompt state
- final assistant reply
- execution outcome and normalized public failure, when present
- telemetry summary

Current implementation note (2026-03-20):

- the current Phase-1 runner captures:
  - benchmark identity
  - session and turn ids
  - copied eval-case timing metadata for session start, turn times, and evaluation reference time
  - rendered prompt artifacts for the evaluated turn
  - structured pre-turn and post-turn mid-term snapshots
  - final assistant reply when successful
  - normalized failure when present
  - emitted events for the evaluated turn
- the current runner does not yet capture:
  - a richer benchmark event timeline beyond the copied eval-case turn inputs
  - explicit prompt-visible mid-term rendering as a separately stored artifact
  - mid-term expansion traces
  - mid-term flush/compaction traces
  - telemetry summary aggregation beyond what is already available through the underlying sink

The runner MAY later capture:

- provider request/response metadata
- tool definitions exposed for the turn
- tool calls and tool results
- provider output items from a tool-calling loop

For memory-focused benchmarks, rendered prompt text alone is not sufficient for debugging or
failure analysis. The eval framework SHOULD capture both:

- prompt-visible memory artifacts
  - what the model could directly see in the prompt
- structured memory state artifacts
  - the internal state that explains why the prompt looked the way it did

Tier 1 detail SHOULD be captured selectively rather than indiscriminately. Recommended default:

- capture metadata for all mid-term episodes
- capture full Tier 1 detail for episodes that were actually expanded
- optionally capture full Tier 1 detail for benchmark-relevant episodes or explicit debug runs

### Time Model

Evaluation MUST treat time as an explicit input, not an accidental property of the host machine.

Three different clocks exist and MUST remain distinct:

- benchmark event time
  - when the replayed session says each message or memory event happened
- benchmark evaluation reference time
  - the replayed session's effective "now" for the evaluated question
- runtime/telemetry time
  - real execution time used for latency, traces, and local diagnostics

Rules:

- memory timestamps in benchmark replay MUST come from benchmark event time
- benchmark evaluation reference time SHOULD be carried as part of replayed eval chronology and
  stored in structured artifacts
- the eval framework SHOULD NOT inject benchmark-only prompt context solely to compensate for a
  product limitation that does not yet exist in the real system
- telemetry and latency recording MUST continue to use runtime clock sources
- evaluation MUST NOT reuse wall-clock `NowTimestamp()` for benchmark chronology once time-aware
  replay support lands
- benchmark timeline artifacts SHOULD represent canonical replayed session history rather than
  splitting entries into benchmark-only versus runtime-observed classes

## Benchmark-First Organization

The first implementation SHOULD organize evaluation work by benchmark.

Recommended steady-state shape:

- one shared eval core
- one adapter per benchmark
- one benchmark config that chooses which autoraters run
- one benchmark-grouped report

Initial benchmark targets:

- `locomo`
- `longmemeval`
- `isla_custom_memory`

Likely later benchmarks:

- `isla_holistic`
- tool-use and exact-detail expansion benchmarks after the live tool path is complete

## Phases

## Phase 0: Evaluation Baseline + Written Contract

### Goal

Freeze the first evaluation architecture and scope before implementation begins.

### Scope

- Publish this phased plan as the canonical evaluation roadmap.
- Explicitly choose benchmark-first organization for the initial implementation.
- Freeze the first evaluation priorities:
  - memory behavior
  - final answer quality
- Freeze the time model:
  - benchmark event time
  - benchmark evaluation reference time
  - runtime telemetry time
- Document the initial non-goals and deferred surfaces.

### Exit Criteria

- A written evaluation roadmap exists in `docs/ai`.
- Time-aware replay is documented as a required early phase, not an optional follow-up.
- The repository has one clear source of truth for evaluation sequencing.

## Phase 1: Eval Core + Artifact Capture

> [!NOTE]
> **Status (2026-03-20): Partially implemented.**
> - The repo now has a small eval core and a working `gateway_app_boundary` runner.
> - The runner currently executes through the real responder path and captures:
>   - rendered prompt artifacts for the evaluated turn
>   - structured mid-term snapshots before and after the evaluated turn
>   - emitted turn events
>   - final reply or normalized failure
> - The current slice intentionally stops short of:
>   - benchmark adapters
>   - standalone eval binaries
>   - time-aware replay
>   - autoraters
>   - full expansion/flush/compaction trace capture
> - Key implemented files:
>   - `server/include/isla/server/evals/eval_types.hpp`
>   - `server/include/isla/server/evals/eval_runner.hpp`
>   - `server/src/evals/eval_runner.cpp`
>   - `server/src/evals/eval_runner_test.cpp`
> - Supporting responder seam added:
>   - `GatewayStubResponder::SnapshotSessionWorkingMemoryState(...)`

### Goal

Add a minimal generic evaluation core that can execute Isla cases and capture reusable artifacts.

### Scope

- Introduce a small internal eval core with concepts equivalent to:
  - case input
  - execution artifacts
  - rater result
  - benchmark report
- Keep the core intentionally small and provider-neutral.
- Explicitly adopt `gateway_app_boundary` as the canonical benchmark execution mode.
- Run cases through the existing responder/application path instead of creating a fake
  evaluation-only path that bypasses the real gateway and memory layers.
- Define the smaller non-default execution modes:
  - `transport_e2e` for protocol and ingress smoke coverage
  - `component` for lower-level targeted debugging
- Capture the canonical artifacts listed above.
- Preserve benchmark identity as first-class metadata on every case and report.

### Design Notes

- The eval core SHOULD be generic.
- The top-level organization SHOULD still remain benchmark-first.
- The core SHOULD be able to run without online benchmark adapters so small local hand-authored
  datasets can land first.
- Phase 1 SHOULD make the injection-point decision explicit in code and documentation instead of
  leaving downstream phases to infer it.

### Exit Criteria

- The repository can execute a case through the current Isla turn path and save structured
  artifacts.
- Artifacts are rich enough to support more than one autorater without rerunning the turn.
- The canonical benchmark execution mode is explicitly defined and implemented.
- Remaining work before Phase 1 is fully complete:
  - capture the rest of the canonical artifact set that is still missing from the current runner
  - decide whether a small standalone eval CLI belongs in Phase 1 or should wait until Phase 2
  - add a first local benchmark surface beyond unit-style runner tests

## Phase 2: Time-Aware Replay + Benchmark Clock Injection

> [!NOTE]
> **Status (2026-03-20): Implemented for the evaluation pipeline.**
> - The runner now supports optional benchmark-supplied:
>   - session start time
>   - per-turn user message time
>   - per-turn assistant reply time
>   - evaluation reference time
> - The responder now resolves session and conversation timestamps through a reusable session-clock
>   seam for the eval path, while the default production path still falls back to wall-clock time.
> - Mid-term stub timestamps now derive from conversation chronology instead of wall-clock time.
> - Setup turns are replayed directly into responder-owned session memory instead of being forced
>   through synthetic runtime-generated setup replies.
> - The evaluated turn remains the live executed turn through the real responder path.
> - Key implemented files:
>   - `server/include/isla/server/evals/eval_types.hpp`
>   - `server/src/evals/eval_runner.cpp`
>   - `server/include/isla/server/ai_gateway_stub_responder.hpp`
>   - `server/src/ai_gateway_stub_responder.cpp`
>   - `server/memory/src/memory_orchestrator.cpp`
>   - `server/src/evals/eval_runner_test.cpp`
> - This phase does not require product-visible support for a benchmark "current/reference time"
>   concept in the serving path. That remains a separate product/runtime concern and SHOULD land
>   as real serving functionality rather than as eval-only prompt injection.

### Goal

Teach the live evaluation path to replay benchmark chronology faithfully instead of relying on wall
 clock time.

### Scope

- Add a time-control seam for session start, user turn time, assistant reply time, and any other
  prompt-visible memory event timestamps.
- Replace benchmark-path calls to wall-clock `NowTimestamp()` with benchmark-supplied event time.
- Preserve benchmark evaluation reference time as part of replayed eval chronology and as a
  structured artifact, without injecting eval-only prompt context that the real product does not
  yet expose.
- Preserve steady-clock telemetry for runtime phase timing.
- Ensure mid-term episode and episode-stub timestamps remain deterministic under replay.

### Design Notes

- Time injection SHOULD happen at the gateway/responder and memory-orchestrator seam, not by
  patching timestamps after execution.
- This phase establishes the generic time-aware replay contract that later benchmark adapters such
  as LOCOMO and LongMemEval will consume.
- If the real product later gains a user-facing notion of current/reference time for temporal
  memory questions, evals SHOULD use that real mechanism instead of an eval-only prompt patch.
- The current Phase-1 runner already proves the canonical app-boundary execution path; Phase 2
  SHOULD extend that runner rather than introducing a second benchmark execution stack.
- The current runner's structured mid-term snapshots make it possible to verify benchmark-time
  propagation once timestamp injection lands.

### Exit Criteria

- The evaluation runner can replay a benchmark timeline with supplied timestamps.
- Prompt-rendered conversation and mid-term memory reflect benchmark chronology.
- Telemetry remains based on runtime clock rather than benchmark clock.
- Benchmark adapters remain future work in later phases and are not required to close this phase.
- Product-visible reference/current-time semantics remain a separate serving-path feature and are
  not required to close this phase.

## Phase 3: Local Memory Evaluation Set + Runner Hardening

> [!NOTE]
> **Status (2026-03-20): Implemented.**
> - Implemented artifacts:
>   - `server/include/isla/server/evals/eval_json.hpp`
>   - `server/include/isla/server/evals/isla_custom_memory_benchmark.hpp`
>   - `server/src/evals/eval_json.cpp`
>   - `server/src/evals/isla_custom_memory_benchmark.cpp`
>   - `server/src/evals/isla_custom_memory_main.cpp`
>   - `server/src/evals/isla_custom_memory_benchmark_test.cpp`
> - Implemented behavior:
>   - a local `isla_custom_memory` benchmark now runs through the canonical app-boundary eval
>     runner with deterministic fake-provider logic at the model/provider seam
>   - the local benchmark persists per-case artifact JSON and a benchmark summary report
>   - the benchmark exercises recall, recency, mid-term visibility, live exact-detail expansion,
>     and benchmark-owned timestamp replay
>   - benchmark setup state is replayed directly into session memory instead of generating
>     synthetic setup replies
> - Remaining limitation:
>   - the generic eval runner still does not capture dedicated tool-loop or flush/compaction trace
>     artifacts outside the persisted local benchmark outputs

### Goal

Land a small Isla-owned benchmark first so the framework can mature before external benchmark
integration.

### Scope

- Add a small hand-authored `isla_custom_memory` benchmark with cases that stress:
  - direct fact recall
  - recency and contradiction handling
  - mid-term episode visibility
  - expandable exact-detail behavior once tool wiring is live
  - relative-time reasoning with explicit benchmark clocks
- Harden artifact persistence and replay diagnostics.
- Add deterministic fake-provider coverage where useful so infrastructure bugs are isolated from
  upstream model variance.
- Reuse the current Phase-1 runner tests as infrastructure regression coverage, but do not treat
  them as the local benchmark itself.

### Exit Criteria

- The repository has a local benchmark that can be run repeatedly during development.
- The framework can catch obvious memory regressions before external benchmark integration lands.

## Phase 3.1: Replay Fidelity Alignment

> [!NOTE]
> **Status (2026-03-20): Implemented.**
> - Implemented artifacts:
>   - `server/include/isla/server/ai_gateway_stub_responder.hpp`
>   - `server/src/ai_gateway_stub_responder.cpp`
>   - `server/include/isla/server/evals/eval_types.hpp`
>   - `server/src/evals/eval_runner.cpp`
>   - `server/src/evals/eval_runner_test.cpp`
> - Implemented behavior:
>   - the responder now consumes one replay/session clock object instead of separate timestamp
>     override callbacks
>   - eval artifacts now describe canonical replayed session history directly
>   - eval persistence uses benchmark-derived `memory_user_id` values so replay traffic is easy to
>     distinguish from product traffic
>   - evaluation reference time is carried as replayed session chronology rather than only passive
>     report metadata
> - Remaining limitation:
>   - the real serving prompt path still does not expose a product-visible notion of current or
>     reference time, so replay reference time remains a structured artifact rather than prompt
>     input

### Goal

Tighten the evaluation model so replayed benchmark runs look as much like real serving runs as
possible, while still preserving deterministic control over chronology and inputs.

### Scope

- Treat benchmark timeline artifacts as canonical replayed session history rather than splitting
  entries into "real runtime" versus "benchmark-only" classes.
- Introduce explicit eval-specific session or user identity in persistence and logs so replay runs
  are easy to distinguish from product traffic without changing the serving-path semantics.
- Move from timestamp override seams toward a clearer replay-clock or session-clock abstraction that
  both eval replay and future serving-path reference-time features can share.
- Clarify the role of evaluation reference time as replayed session time, not merely passive report
  metadata.
- Keep the evaluated turn on the real responder execution path while continuing to avoid synthetic
  setup replies.

### Design Notes

- Phase 3.1 is a fidelity and cleanup slice, not a benchmark-adapter slice.
- This work SHOULD refine the replay model without rewriting the completed local benchmark work in
  Phase 3.
- If the real product later gains a first-class notion of current/reference time, eval replay
  SHOULD reuse that same mechanism instead of maintaining a separate eval-only concept.

### Exit Criteria

- Eval artifacts describe replayed session history without the old benchmark-only versus
  runtime-observed distinction.
- Eval persistence and logs can be distinguished cleanly from product traffic.
- The next replay-clock or session-clock seam is documented clearly enough to guide follow-up
  implementation work.

## Phase 3.2: Real Main-Turn LLM Execution

> [!NOTE]
> **Status (2026-03-21): Implemented.**
> - Implemented artifacts:
>   - `server/include/isla/server/evals/isla_custom_memory_benchmark.hpp`
>   - `server/src/evals/isla_custom_memory_benchmark.cpp`
>   - `server/src/evals/isla_custom_memory_main.cpp`
>   - `server/src/evals/isla_custom_memory_benchmark_test.cpp`
> - Implemented behavior:
>   - the evaluated benchmark turn now runs through the real OpenAI Responses main LLM path
>   - the standalone benchmark binary reuses the shared gateway startup-config parsing and OpenAI
>     environment/flag handling
>   - benchmark setup replay still enters through session memory rather than synthetic setup
>     replies
>   - benchmark-local stubbing now remains only around mid-term decider/compactor behavior
> - Remaining limitation:
>   - the local benchmark no longer preserves a deterministic fake main-turn mode; non-live
>     infrastructure-only regression work should use lower-level unit tests rather than the
>     benchmark surface

### Goal

Replace deterministic fake-provider handling of the evaluated turn with the real main LLM call path
while preserving the existing replay and artifact-capture model.

### Scope

- Execute the evaluated turn through the same real provider-backed main LLM path used by serving
  instead of returning benchmark-owned fake main-turn completions.
- Keep benchmark setup replay behavior unchanged so setup state still enters through replayed
  session memory rather than synthetic setup replies.
- Preserve the current artifact capture contract:
  - replayed benchmark timeline
  - rendered prompt artifacts
  - structured pre/post mid-term snapshots
  - emitted events
  - final reply and normalized failures
- Reuse the shared serving-path OpenAI startup/config parsing for the standalone benchmark entry
  point instead of adding a separate benchmark-only provider configuration path.
- Document any remaining cases that still require benchmark-local stubbing, such as compactor or
  decider isolation while the local benchmark still depends on exact prompt-visible memory shapes.

### Design Notes

- Phase 3.2 is about real evaluated-turn execution, not about external benchmark adapters.
- The important boundary is that the main user-facing turn should use the real provider path; setup
  replay and benchmark chronology control should remain benchmark-owned.
- This phase SHOULD make it obvious which parts of a run are true serving-path behavior and which
  parts are still intentionally stubbed for benchmark-shape stability.

### Exit Criteria

- The local benchmark can run the evaluated turn through the real main LLM call path.
- Artifact output remains stable enough to support later autoraters and external benchmark adapters.

## Phase 3.3: Backend Runtime Turn Seam

> [!NOTE]
> **Status (2026-03-22): Implemented.**
> - Implemented artifacts:
>   - `server/include/isla/server/ai_gateway_stub_responder.hpp`
>   - `server/src/ai_gateway_stub_responder.cpp`
>   - `server/include/isla/server/evals/eval_runner.hpp`
>   - `server/src/evals/eval_runner.cpp`
> - Implemented behavior:
>   - `GatewayStubResponder` now exposes a direct accepted-turn execution seam that runs one turn
>     to terminal outcome and returns a structured result.
>   - the responder worker path and the eval runner now share the same terminal turn execution
>     logic instead of maintaining separate control-flow models.
>   - the eval runner still records emitted session events for artifacts, but it no longer waits on
>     recorded terminal events to know whether a turn finished.
>   - the eval runner's former `event_timeout` is now a `session_settle_timeout`, clarifying that
>     the remaining timeout only bounds working-memory snapshot settling rather than live turn
>     completion.

### Goal

Introduce a backend-owned turn-execution seam that both serving and evaluation can share, so
benchmark runs can await completed turns directly instead of inferring completion indirectly from
event-emission side effects.

### Scope

- Define a backend-owned runnable turn API that executes one replayed turn through the real backend
  pipeline and returns only when the turn reaches a terminal outcome.
- Keep the canonical benchmark execution path aligned with real backend wiring rather than relying
  on benchmark-only orchestration logic layered over the responder event sink.
- Preserve `gateway_app_boundary` realism where it is still useful, but stop treating callback
  terminalization and short event waits as the long-term benchmark control model.
- Keep transport/WebSocket execution as a separate higher-cost path rather than making a live
  server process the default benchmark requirement.
- Clarify which artifact capture responsibilities belong to the backend-owned turn seam versus the
  surrounding benchmark runner.

### Design Notes

- This sub-phase is about control-flow and ownership fidelity, not about adding new benchmarks.
- The problem to solve is that `OnTurnAccepted(...)` is an event-ingress seam: it queues work and
  returns before the turn is finished, which forces evals to wait indirectly on emitted events.
- A backend-owned turn seam SHOULD allow evals to block on true turn completion without inventing a
  benchmark-only execution stack.
- The serving path and the benchmark path SHOULD share as much startup, config, and execution
  wiring as practical once this seam exists.

### Exit Criteria

- A backend-owned turn execution seam is documented and implemented clearly enough that benchmark
  runs can await terminal turn outcomes directly.
- Later benchmark work no longer depends on short runner-level event timeouts to distinguish slow
  turns from broken orchestration.
- The phased plan makes this seam a prerequisite for later benchmark and autorater expansion.

## Phase 3.4: Deterministic Post-Turn Memory Settling

> [!NOTE]
> **Status (2026-03-22): Implemented.**
> - Implemented artifacts:
>   - `server/memory/include/isla/server/memory/memory_orchestrator.hpp`
>   - `server/memory/src/memory_orchestrator.cpp`
>   - `server/include/isla/server/ai_gateway_stub_responder.hpp`
>   - `server/src/ai_gateway_stub_responder.cpp`
>   - `server/include/isla/server/evals/eval_runner.hpp`
>   - `server/src/evals/eval_runner.cpp`
> - Implemented behavior:
>   - `MemoryOrchestrator` now exposes `AwaitAndDrainAllPendingMidTermCompactions()` which blocks
>     on all pending mid-term futures before draining, replacing timeout-based polling.
>   - `GatewayStubResponder` now exposes `AwaitSessionMemorySettled()` which blocks until all
>     pending mid-term memory work for the session has completed and been applied.
>   - `SnapshotSessionWorkingMemoryState()` no longer accepts a settle timeout parameter and is
>     now a pure snapshot operation.
>   - `EvalRunnerConfig::session_settle_timeout` has been removed; the eval runner now calls
>     `AwaitSessionMemorySettled()` before each memory snapshot instead.
>   - the `isla_custom_memory` benchmark no longer specifies a settle timeout.

### Goal

Remove benchmark dependence on hard-coded post-turn memory settle timeouts by giving evals a
backend-owned way to block until relevant mid-term memory work for the current turn has actually
reached a settled state.

### Scope

- Replace timeout-based post-turn memory snapshot settling with an explicit completion or wait
  model for pending mid-term compactions and related turn-scoped memory follow-up work.
- Add a responder or memory-orchestrator seam that lets evals wait for session memory to become
  observably settled without guessing a deadline.
- Keep this work focused on correctness and replay fidelity rather than on throughput
  optimization.
- Preserve the ability to keep bounded waits or defensive fallbacks in production-serving paths
  where indefinite blocking would be undesirable.

### Design Notes

- Phase 3.3 fixed turn completion ownership, but post-turn artifact capture still uses a
  `session_settle_timeout` while snapshotting memory state.
- That timeout is no longer waiting for the main LLM response; it is waiting for pending mid-term
  memory work to finish draining before eval artifacts are captured.
- This can still create avoidable flakiness or force overly conservative benchmark defaults if the
  settle contract remains timeout-based.
- The preferred long-term model is an explicit "memory settled" signal or waitable condition, not
  a larger timeout constant.

### Exit Criteria

- Eval artifact capture no longer depends primarily on a hard-coded post-turn settle timeout.
- The benchmark runner can await post-turn memory settling through an explicit backend-owned seam.
- Timeout-based memory settling, if still present as a fallback, is clearly secondary rather than
  the canonical eval control model.

## Phase 4: Generic Memory Benchmark Runner

### Goal

Extract a shared benchmark runner that any external dataset adapter can plug into, so adding a new
benchmark requires only a normalization adapter and a thin CLI entry point.

### Scope

- Introduce `MemoryBenchmarkCase` (eval case + opaque JSON metadata) and `MemoryBenchmarkSuite`
  (named collection of cases) as the canonical adapter-to-runner interface.
- Implement `RunMemoryBenchmark` which owns the shared run loop: OpenAI client setup, output
  directory creation, per-case `EvalRunner::RunCase` execution, per-case artifact writing, and
  aggregate report generation.
- Expose `BuildMemoryBenchmarkReportJson` for unit-testable report serialization.
- Metadata flows through from adapter input to per-case artifacts and the aggregate report without
  the runner interpreting it.

### Design Notes

- Adapters SHOULD normalize source data into `MemoryBenchmarkSuite` and call `RunMemoryBenchmark`.
- Adapters SHOULD NOT duplicate run-loop, artifact-writing, or report-building logic.
- The `isla_custom_memory` benchmark retains its own runner because it uses per-case OpenAI client
  wrapping for deterministic mid-term stubbing, which is a fundamentally different pattern from
  external dataset benchmarks.

### Exit Criteria

- `RunMemoryBenchmark` can execute an arbitrary `MemoryBenchmarkSuite` end-to-end.
- Report and artifact serialization are covered by unit tests.
- Adding a new external benchmark requires only an adapter that returns `MemoryBenchmarkSuite`.

## Phase 5: LongMemEval Adapter

### Goal

Add an adapter for LongMemEval-S that normalizes the dataset into `MemoryBenchmarkSuite` and runs
it through the generic memory benchmark runner.

### Scope

- Load and parse the `longmemeval_s_cleaned.json` dataset.
- Flatten `haystack_sessions` into the canonical conversation format with session-level timestamps
  from `haystack_dates`.
- Handle mixed answer types (string and integer) and date-only timestamp formats.
- Preserve benchmark-specific metadata: `question_type`, `answer_session_ids`, and per-turn
  `has_answer` indices for future autorater retrieval evaluation.
- Support deterministic sampling with a configurable rate and seed to control token usage during
  development.
- Add a standalone `longmemeval_eval` binary with CLI flags for dataset path, sample rate, random
  seed, and case ID filtering.

### Design Notes

- LongMemEval timestamps are session-level only; individual turns within a session share the same
  timestamp. Replay ordering is guaranteed by sequential execution, not by timestamps.
- The adapter validates entries through `BuildEvalCaseFromBenchmarkTimeline` to catch structural
  issues (e.g., assistant messages without preceding user messages) before runtime.
- A 5% default sample rate keeps token costs manageable for iterative development.

### Exit Criteria

- LongMemEval-S cases can be executed through the Isla eval runner via `RunMemoryBenchmark`.
- Adapter parsing and normalization are covered by unit tests.
- Known fidelity gaps, if any, are written down instead of silently ignored.

## Phase 5.1 (Optional): LoCoMo Adapter

### Goal

Add an adapter for LoCoMo that normalizes the dataset into `MemoryBenchmarkSuite` and runs it
through the generic memory benchmark runner.

### Scope

- Load and parse the LoCoMo dataset.
- Normalize LoCoMo's conversation and question format into the canonical eval case shape.
- Preserve LoCoMo-specific metadata needed for reporting.
- Add a standalone `locomo_eval` binary.

### Design Notes

- LoCoMo's exact schema and any structural quirks should be investigated during implementation.
- The adapter follows the same pattern established by the LongMemEval adapter in Phase 5.

### Exit Criteria

- LoCoMo cases can be executed through the Isla eval runner via `RunMemoryBenchmark`.
- Known fidelity gaps, if any, are written down instead of silently ignored.

## Phase 6: Multiple Autoraters For Memory + Final Answer

### Goal

Support multiple autoraters per benchmark, with memory and final answer as the first required
dimensions.

### Scope

- Add a memory-focused autorater that judges:
  - correct remembered facts
  - contradiction against available memory
  - unsupported fabrication
  - temporal faithfulness to the benchmark timeline
- Add a final-answer autorater that judges:
  - usefulness
  - directness
  - instruction following
  - answer quality independent of whether memory succeeded
- Allow one benchmark run to execute both raters over the same artifacts.
- Store structured rater outputs with:
  - score
  - pass/fail or bucket
  - short rationale
  - machine-readable evidence fields

### Design Notes

- The first raters SHOULD operate on captured artifacts rather than driving a second live Isla turn.
- Rater outputs SHOULD use strict structured JSON so benchmark reports stay machine-readable.
- The framework SHOULD allow benchmark-specific rater configuration without requiring every
  benchmark to use the same judge prompt.

### Exit Criteria

- One benchmark run can produce more than one rater result.
- Memory and final answer can be inspected separately for the same case.

## Phase 6.5: Judge Calibration + Failure Analysis Workflow

### Goal

Make autorater output trustworthy enough to drive iteration instead of becoming opaque noise.

### Scope

- Add a small labeled review set for spot-checking judge behavior.
- Record disagreement examples and obvious systematic judge failures.
- Add failure-analysis output that makes it easy to inspect:
  - benchmark input timeline
  - rendered memory context
  - final assistant reply
  - rater rationales
- Document how to compare judge score movement against known benchmark cases before using results
  as a hard regression gate.

### Exit Criteria

- The repository has a repeatable judge sanity-check workflow.
- Developers can inspect why a case failed without manually reconstructing the entire turn.

## Phase 7: Reporting + Regression Gates

### Goal

Turn eval runs into useful regression signals for daily development and model/runtime changes.

### Scope

- Add benchmark-grouped summaries.
- Report at least:
  - benchmark-level pass rates
  - benchmark-level average scores
  - per-rater aggregates within each benchmark
  - notable failing cases
- Add a lightweight gating policy for selected benchmarks.
- Keep the first gates advisory or soft-fail until judge calibration is stable.

### Design Notes

- Gating SHOULD begin with the smallest reliable benchmark slices.
- Soft regression warnings are preferable to brittle hard failures in the first rollout.
- Benchmark-level reporting SHOULD remain the default user-facing presentation.

### Exit Criteria

- Eval runs can be used to compare two changes meaningfully.
- The repository has at least one practical regression gate tied to benchmark output.

## Phase 8: Holistic And Future Evaluation Surfaces

### Goal

Extend the framework beyond the initial memory + final-answer focus once the corresponding runtime
surfaces are mature.

### Scope

- Add holistic benchmark views that consider memory behavior and final answer together.
- Add tool-use evaluation after the live tool loop is complete.
- Add exact-detail expansion evaluation after `expand_mid_term` is fully wired into live session
  memory.
- Add long-term retrieval evaluation once the implemented runtime goes beyond working and mid-term
  memory.

### Exit Criteria

- The framework can evaluate more than the current v1 text-only responder path without rewriting
  the core evaluation architecture.

## Optional Phase 9: Parallel Evaluation Execution

### Goal

Improve evaluation throughput by running multiple benchmark cases concurrently instead of forcing
all eval execution through one serial path.

### Scope

- Add optional support for executing multiple eval cases at once.
- Prefer parallelism across isolated eval sessions rather than concurrent turns within one live
  session, so the production v1 one-turn-in-flight session invariant remains unchanged.
- Allow the eval runner to bound concurrency explicitly instead of spawning unbounded parallel
  work.
- Ensure artifact capture, logging, and report assembly remain deterministic even when cases finish
  out of order.
- Ensure benchmark time injection remains case-local so one replayed timeline cannot bleed into
  another.

### Design Notes

- This phase is intentionally optional because correctness, time fidelity, and judge trust matter
  more than throughput in the initial rollout.
- Parallel evaluation SHOULD be implemented at the runner level, not by weakening the gateway's
  session lifecycle guarantees.
- If the current single-worker responder becomes a bottleneck for eval throughput, parallel eval
  infrastructure SHOULD either:
  - create isolated responder/session instances per case, or
  - add an explicit eval-only concurrency seam that preserves deterministic artifact capture.

### Exit Criteria

- The eval runner can execute multiple benchmark cases concurrently with a configurable concurrency
  limit.
- Parallel runs produce the same per-case artifacts and scores as serial runs, modulo expected
  judge variance.
- The gateway's existing production session invariants remain intact.

## Sequencing Summary

Recommended implementation order:

1. Phase 0
2. Phase 1
3. Phase 2
4. Phase 3
5. Phase 3.1
6. Phase 3.2
7. Phase 3.3
8. Phase 3.4
9. Phase 4
10. Phase 5
11. Optional Phase 5.1, only when LoCoMo coverage is needed
12. Phase 6
13. Phase 6.5
14. Phase 7
15. Phase 8
16. Optional Phase 9, only when eval throughput becomes a practical bottleneck

The critical-path rule is:

- do not treat external memory benchmark numbers as authoritative until Phase 2 is complete
- do not block Phase 2 completion on future benchmark adapters or on separate serving-path support
  for a user-facing reference/current-time concept
- do not continue deeper benchmark and autorater implementation past the current local benchmark
  slice until Phase 3.3 has established the backend-owned turn seam clearly enough
- do not treat timeout-based post-turn memory settling as a stable long-term benchmark contract
  once Phase 3.4 work begins
- do not use autoraters as hard regression gates until Phase 6.5 has produced enough calibration
  confidence

## Changelog

- 2026-03-23: completed Phase 4 by extracting a generic `MemoryBenchmarkRunner` with
  `MemoryBenchmarkCase`, `MemoryBenchmarkSuite`, and `RunMemoryBenchmark` that owns the shared run
  loop, artifact writing, and report generation. Split the previous Phase 4 into Phase 4
  (infrastructure), Phase 5 (LongMemEval adapter), and Phase 5.1 (optional LoCoMo adapter) so
  that adding a new external benchmark requires only a normalization adapter and a thin CLI entry
  point.
- 2026-03-22: completed Phase 3.4 by replacing the timeout-based post-turn memory settling loop
  with explicit blocking awaits through `AwaitAndDrainAllPendingMidTermCompactions` on the memory
  orchestrator and `AwaitSessionMemorySettled` on the responder, removing `session_settle_timeout`
  from the eval runner config, and simplifying `SnapshotSessionWorkingMemoryState` to a pure
  snapshot without timeout parameters.
- 2026-03-22: added Phase 3.4 to make explicit post-turn memory settling a first-class follow-up,
  so eval artifact capture can stop depending on a conservative hard-coded settle timeout.
- 2026-03-22: completed Phase 3.3 by adding a responder-owned direct accepted-turn execution seam,
  reusing that same terminal execution path from the serving worker loop and the eval runner, and
  removing eval runner dependence on recorded terminal session events for turn completion.
- 2026-03-21: added Phase 3.3 to make the backend-owned turn execution seam explicit before
  proceeding with later benchmark and autorater implementation, so evals can eventually block on
  true turn completion instead of waiting indirectly on responder-emitted terminal events.
- 2026-03-21: completed Phase 3.2 by routing `isla_custom_memory` evaluated turns through the real
  OpenAI Responses path, reusing the shared gateway startup-config parser in the benchmark binary,
  and retiring the benchmark's fake main-turn fallback while retaining benchmark-local
  decider/compactor stubbing.
- 2026-03-20: completed Phase 3.1 by replacing responder-side timestamp override callbacks with a
  shared session-clock seam, renaming eval replay artifacts around canonical replayed session
  history, and assigning eval-specific benchmark-derived `memory_user_id` values to replay runs.
- 2026-03-20: added Phase 3.2 to make the planned switch from deterministic fake-provider
  evaluated-turn execution to the real main LLM call path explicit before Phase 4 benchmark
  adapters.
- 2026-03-20: updated the phased plan to reflect the current replay model more accurately,
  clarified that setup turns are replayed directly into session memory while the evaluated turn
  remains live on the responder path, reframed evaluation reference time as replay chronology
  rather than passive metadata, and added Phase 3.1 for replay-fidelity alignment work.
- 2026-03-20: completed Phase 3 by adding a runnable `isla_custom_memory` benchmark, deterministic
  fake-provider coverage over the real app-boundary eval runner, persisted per-case artifact JSON
  plus benchmark summary output, a standalone `isla_custom_memory_eval` binary, and focused
  regression coverage for the new local benchmark slice.
- 2026-03-20: extended the Phase 2 eval slice with a minimal benchmark-timeline adapter utility
  that normalizes benchmark-owned turn timelines into `EvalCase`, added a normalized benchmark
  event timeline artifact covering session start, user/assistant chronology, and evaluation
  reference time, and expanded eval-runner coverage for the new adapter and artifact behavior.
- 2026-03-20: completed the first implementation slice of Phase 2 by adding optional
  benchmark-supplied session and turn timestamps to the eval runner, keeping evaluation reference
  time as structured metadata rather than eval-only prompt context, routing responder-side
  timestamp resolution through overrideable seams for eval execution, aligning mid-term stub
  timestamps with conversation chronology, and adding focused regression coverage for prompt and
  persistence-side time fidelity.
- 2026-03-20: completed the first implementation slice of Phase 1 by adding a small
  benchmark-first eval core, a canonical app-boundary runner over `GatewayStubResponder`, prompt
  artifact capture for the evaluated turn, structured pre/post mid-term snapshots, emitted-event
  capture, and focused regression coverage; updated the plan to distinguish what Phase 1 now
  provides versus the remaining work still deferred to later phases.
- 2026-03-19: added the initial evaluation phased plan covering benchmark-first organization,
  time-aware replay requirements for memory benchmarks, multiple autoraters for memory and final
  answer quality, benchmark adapter sequencing, and later holistic/tool-use expansion.
