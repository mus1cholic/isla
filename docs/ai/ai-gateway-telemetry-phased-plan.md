# AI Gateway Telemetry Phased Plan

Last updated: 2026-03-14

## Purpose

This document freezes the initial telemetry baseline for the `isla` AI gateway and memory-backed
turn pipeline.

It is the authoritative Phase 0 reference for:

- turn-latency measurement goals
- latency-segment boundaries
- trace and metric naming
- privacy and cardinality rules
- clock and timestamp policy
- phased implementation sequencing

Later telemetry work should extend this document instead of redefining observability contracts in
chat, code comments, or ad hoc dashboards.

## Relationship To Existing Docs

This plan builds on the current architecture and memory references rather than replacing them:

- `docs/ai/ai_gateway_v1_design.md` remains the authoritative v1 gateway ownership and turn-flow
  baseline.
- `docs/ai/ai-gateway-phased-plan.md` remains the authoritative implementation history and gateway
  rollout plan.
- `docs/memory/memory-planning.md` remains the authoritative working/mid-term/long-term memory
  architecture reference.

Telemetry work MUST preserve the boundaries already established in those documents.

## Context

`isla` currently has:

- a runnable WebSocket-based gateway server
- a transport-facing session handler
- an application-owned `GatewayStubResponder`
- a planner/executor boundary
- a live OpenAI Responses provider path behind the executor seam
- a working-memory and mid-term-memory path integrated into the responder

The current implementation also has an explicit known limitation:

- `GatewayStubResponder` still uses one blocking worker across all sessions, so slow execution or
  slow accepted-turn emit completion can delay unrelated sessions

That limitation makes latency telemetry a first-order need rather than a later polish task.

## Goals

The first telemetry slice should answer these questions reliably:

- What are the p50, p95, and p99 latencies for a successful accepted turn?
- What are the p50, p95, and p99 latencies for each major segment of the turn pipeline?
- How much time is spent waiting in the responder queue versus executing work?
- How long does the provider leg take, including time to first token and full streamed completion?
- How much time is spent in memory shaping and memory write-back?
- How much time is spent waiting on server-owned outbound emits?
- When a regression appears, which segment moved?

## Non-Goals

Phase 0 intentionally does not define:

- client-visible partial-text telemetry for streamed UI updates, since client-visible partial text is
  still out of scope for v1
- full profiling or flamegraph capture
- prompt/body content capture in traces or metrics
- vendor-specific dashboard lock-in
- a production-only collector topology
- long-term memory retrieval telemetry beyond the current implemented surfaces

## Architecture Constraints

The telemetry contract MUST respect the current architecture:

- The desktop client talks only to the application-owned gateway.
- The WebSocket/session layer does not own OpenAI I/O.
- The planner emits one execution plan in v1.
- The executor performs one OpenAI request in v1.
- The current planner/executor path remains intentionally independent of memory shaping.
- The memory system remains a responder concern.
- Async emit completion means transport-executor acceptance or rejection, not remote socket flush.
- One accepted turn is in flight per session in v1.

The telemetry contract MUST also reflect the written memory design:

- Working memory is intended to preserve immediate, low-latency interaction.
- Mid-term memory writes should remain fast and simple at runtime.
- Mid-term Tier 2 summaries are always present in context.
- Long-term retrieval is a separate staged concern and should be instrumented as it lands.
- Async memory writes should remain non-blocking from the user's perspective.

## Recommendation

Implement telemetry in two layers:

- traces for per-turn debugging and latency-breakdown inspection
- histograms for p50/p95/p99 regressions and dashboards

The implementation SHOULD be vendor-neutral at the code boundary:

- a local `TelemetrySink` seam in Isla code
- a no-op implementation available by default
- an OpenTelemetry-backed adapter added later without forcing the rest of the gateway to depend on
  a specific exporter or backend

OpenTelemetry is the recommended export format and tracing/metrics SDK for later phases, but the
Phase 0 baseline should remain valid even if export plumbing changes.

## Telemetry Model

### Root Unit

The root unit of latency is one accepted turn.

The root trace/span for an accepted turn is:

- `isla.gateway.turn`

The root latency histogram is:

- `isla.gateway.turn.duration_ms`

Root timing begins when `text.input` has been validated and accepted as a `TurnAcceptedEvent`.

Root timing ends when the server has completed the final server-owned terminal emit for that turn:

- successful path: after `turn.completed` is accepted by the transport executor
- failed path: after `error` + terminal completion/cancellation handling finishes
- cancelled path: after `turn.cancelled` is accepted by the transport executor

Because the current emit callback does not guarantee remote socket flush, this server-side total is
not yet the same as final user-perceived latency.

### Segment Unit

The root turn span contains child spans for major pipeline segments. Segment latencies are also
recorded in a shared histogram:

- `isla.gateway.phase.duration_ms`

Each histogram point MUST include a low-cardinality `phase` attribute.

### First Provider Signal

Time to first provider token is tracked separately:

- `isla.gateway.provider.first_token_ms`

This metric exists because first-token latency is often the clearest signal for upstream provider or
network regressions.

## Canonical Phase Boundaries

Phase names below are the canonical initial latency slices for the current v1 implementation.

### Gateway And Responder

- `gateway.accept`
  - parse inbound JSON
  - validate message
  - enforce session-turn state
  - produce `TurnAcceptedEvent`
- `memory.user_query`
  - update working memory for the accepted user turn
  - render memory-backed system/context inputs for execution
- `queue.wait`
  - time between responder enqueue and worker dequeue
  - explicitly captures cross-session head-of-line blocking from the current single worker
- `plan.create`
  - construct the v1 execution plan
- `executor.total`
  - execute the full plan from first step dispatch to final execution result
- `emit.text_output`
  - server-owned `text.output` emit wait
- `memory.assistant_reply`
  - write assistant reply back into memory after successful generation
- `emit.turn_completed`
  - final server-owned completion emit wait
- `emit.error`
  - post-acceptance error emit wait on failed turns
- `emit.turn_cancelled`
  - terminal cancellation emit wait on cancelled turns

### Executor And Provider

- `executor.step`
  - one execution step within the ordered execution plan
- `llm.provider.total`
  - provider request start through final provider completion
- `provider.serialize_request`
  - request-shaping and JSON serialization before transport dispatch
- `provider.transport`
  - total provider-client transport call from dispatch into the OpenAI Responses client until that
    client returns control to the caller
- `provider.stream`
  - streamed SSE response consumption from first response byte through terminal provider event
- `provider.aggregate_text`
  - local buffering and final-output assembly from provider text deltas into the final text result

Relationship rules:

- `llm.provider.total` is the enclosing provider phase.
- `provider.serialize_request` is a sequential precursor to transport dispatch.
- In the long-term target model, `provider.transport` is the enclosing transport phase and
  `provider.stream` plus `provider.aggregate_text` are finer-grained sub-spans within that provider
  leg.
- In the current implementation, these finer-grained phases MAY be emitted sequentially if that
  matches the available instrumentation points more closely than true nesting.
- Dashboards and regressions MUST NOT assume that `provider.transport`, `provider.stream`, and
  `provider.aggregate_text` are always additive sibling durations.
- If nested instrumentation is available, prefer nested spans and treat `provider.transport` as the
  total wall-clock duration of the provider client call.

### Future Memory Retrieval

These phases are reserved for later memory work and SHOULD NOT be faked before the underlying
runtime exists:

- `memory.long_term.collect_candidates`
- `memory.long_term.rerank`
- `memory.long_term.filter`
- `memory.long_term.inject`
- `memory.mid_term.flush_async`

## Canonical Span Events

The following event names are the initial canonical timestamps inside the root turn trace:

- `turn.accepted`
- `memory.user_query.started`
- `memory.user_query.completed`
- `turn.enqueued`
- `turn.dequeued`
- `plan.create.started`
- `plan.create.completed`
- `executor.started`
- `executor.completed`
- `provider.dispatched`
- `provider.first_token`
- `provider.completed`
- `text_output.emit.started`
- `text_output.emit.completed`
- `error.emit.started`
- `error.emit.completed`
- `memory.assistant_reply.started`
- `memory.assistant_reply.completed`
- `turn.cancelled.emit.started`
- `turn.cancelled.emit.completed`
- `turn.completed.emit.started`
- `turn.completed.emit.completed`
- `turn.failed`

Span events SHOULD carry small diagnostic attributes when helpful, such as `step_name`, `model`,
`reasoning_effort`, or `outcome`, but MUST NOT capture prompt text or provider raw bodies.

## Attributes And Cardinality Rules

### Allowed Low-Cardinality Attributes

The initial metric and span attribute set SHOULD be limited to low-cardinality fields:

- `phase`
- `outcome`
- `cancelled`
- `step_name`
- `model`
- `reasoning_effort`
- `transport_backend`
- `status_code_class`

### High-Cardinality Fields

The following fields MUST NOT be metric attributes:

- `session_id`
- `turn_id`
- prompt text
- system prompt text
- working memory content
- provider response text
- raw error messages

These fields MAY appear in traces or logs only when needed for debugging and only through existing
sanitization rules. They SHOULD remain optional and MUST NOT be required to compute latency
aggregates.

## Clock Policy

Latency math MUST use `std::chrono::steady_clock`.

Wall-clock timestamps used for logs, trace correlation, or future exported event timestamps MAY use
`std::chrono::system_clock`.

Durations MUST NOT be derived from wall-clock timestamps. This prevents false regressions if the
system clock jumps.

## Privacy And Safety Rules

Telemetry MUST remain safe to enable in development and production-like environments.

The implementation MUST NOT record:

- prompt text
- working memory dumps
- provider request or response bodies
- user message content
- audio payloads
- full upstream URLs containing secrets
- API keys, project IDs, or bearer tokens

The implementation SHOULD record:

- sanitized step names
- model identifiers
- reasoning effort
- transport backend type
- outcome class
- byte counts when useful for debugging size-related regressions

## Export And Backend Strategy

Phase 0 does not require a concrete backend, but it freezes the intended direction:

- code should emit through a local telemetry seam
- OpenTelemetry should be the first export adapter
- direct export is acceptable for development and small-scale testing
- an OpenTelemetry Collector remains optional at first
- the telemetry seam should not assume a specific hosted vendor

## Testing Expectations

Telemetry work SHOULD land with regression coverage for:

- successful accepted turns
- provider failure after acceptance
- cancellation before provider completion
- oversized input or output rejection paths where timing still matters
- queueing behavior across multiple accepted turns
- no-op sink behavior
- clock-boundary correctness for duration math

Tests SHOULD assert phase presence and sane ordering, not exact wall-clock values.

## Phase 0: Telemetry Baseline

Freeze the telemetry contract before implementing instrumentation.

Deliverables:

- this document
- canonical span names
- canonical histogram names
- attribute and privacy rules
- clock policy
- phased rollout sequence

Out of scope:

- OpenTelemetry dependency wiring
- exporter setup
- collector setup
- dashboard implementation
- code instrumentation

## Phase 1: Local Telemetry Seam

Introduce a local code boundary for observability without committing the gateway to a concrete SDK.

Goals:

- add a narrow `TelemetrySink` interface
- add a no-op implementation
- add a per-turn timing context object shared across gateway, responder, executor, and provider
- keep the business logic transport-agnostic and telemetry-backend-agnostic

Expected first touch points:

- session acceptance path
- responder enqueue/dequeue path
- executor boundary
- provider boundary
- server-owned emit waits

## Phase 2: Gateway And Responder Latency

Instrument the current end-to-end accepted-turn pipeline.

Primary focus:

- `isla.gateway.turn.duration_ms`
- `isla.gateway.phase.duration_ms`

Required segments:

- `gateway.accept`
- `memory.user_query`
- `queue.wait`
- `plan.create`
- `executor.total`
- `emit.text_output`
- `memory.assistant_reply`
- `emit.turn_completed`

This phase should answer whether the current responder worker is the dominant latency source for
tail behavior.

## Phase 3: Executor And Provider Segmentation

Instrument the OpenAI execution leg in more detail.

Primary focus:

- `executor.step`
- `llm.provider.total`
- `provider.serialize_request`
- `provider.transport`
- `provider.stream`
- `provider.aggregate_text`
- `isla.gateway.provider.first_token_ms`

This phase should make it possible to distinguish:

- provider/network slowdown
- request-build overhead
- stream-consumption delay
- local output aggregation overhead
- Linux in-process transport behavior versus native Windows `curl` fallback behavior

## Phase 4: Memory Runtime Expansion

Extend telemetry as the broader memory architecture described in `docs/memory/memory-planning.md`
lands in code.

Primary focus:

- preserve visibility into always-present working-memory and mid-term shaping costs
- add separate timing for long-term candidate collection, re-ranking, filtering, and injection
- add async mid-term flush timing without blocking the main user-visible turn path

This phase should preserve the written memory principle that the user-facing path stays snappy while
write-heavy or background memory work remains observable.

## Phase 5: OpenTelemetry Adapter

Add an OpenTelemetry-backed implementation of the local telemetry seam.

Goals:

- export root and child spans
- export latency histograms
- keep no-op and test implementations available
- support direct export first
- allow optional later Collector usage without changing gateway business logic

## Phase 6: User-Perceived End-To-End Latency

Add client-side instrumentation so server latency can be compared with what the user actually feels.

This phase exists because current server emit completion only means transport-executor
acceptance/rejection, not confirmed remote flush or visible render.

Likely future measurements:

- client send to first server response frame
- client send to final `text.output` receive
- client receive to visible text render
- client receive to audio playback start

## Decision Rules

When a new telemetry point is proposed, prefer the smallest boundary that answers one of these:

- Did total latency regress?
- Which segment moved?
- Is the regression local, provider-side, memory-side, or transport-side?
- Is the regression a median shift or a tail-latency problem?

Do not add telemetry only because a clock is nearby in the code.

## Initial File Targets For Later Phases

The current implementation suggests these first instrumentation targets:

- `server/src/ai_gateway_session_handler.cpp`
- `server/src/ai_gateway_stub_responder.cpp`
- `server/src/ai_gateway_executor.cpp`
- `server/src/ai_gateway_step_registry.cpp`
- `server/src/openai_llms.cpp`
- `server/src/openai_responses_client.cpp`
- `server/src/ai_gateway_server.cpp`

## Changelog

- 2026-03-14: implemented Phase 2 for the current gateway path by recording `gateway.accept`,
  `memory.user_query`, `queue.wait`, `plan.create`, `executor.total`, `emit.text_output`,
  `memory.assistant_reply`, `emit.turn_completed`, `emit.error`, and `emit.turn_cancelled`, along
  with the corresponding canonical timestamp events and terminal turn outcomes through the local
  telemetry seam.
- 2026-03-14: implemented Phase 1 with a local telemetry seam in the gateway codebase, including a
  shared `TurnTelemetryContext`, a default no-op `TelemetrySink`, gateway-boundary context creation
  at accepted-turn time, and propagation of that turn-scoped context through the responder,
  executor, and provider request surfaces.
- 2026-03-14: added the initial telemetry Phase 0 baseline covering root turn timing, canonical
  latency segments, metric/span naming, privacy and cardinality rules, clock policy, and a phased
  implementation path toward an OpenTelemetry-backed export layer and later user-perceived
  end-to-end latency measurement.
