# AI Gateway Protocol-First Streaming Phased Plan

## Context

`isla` currently has:

- A typed client/gateway WebSocket protocol in `shared/include/isla/shared/ai_gateway_protocol.hpp`
- Session/turn lifecycle enforcement in `shared/include/isla/shared/ai_gateway_session.hpp`
- A runnable gateway server and live-session egress boundary in `server/src`
- A live OpenAI Responses integration that already streams upstream SSE events internally
- A stub responder/orchestration path that currently buffers provider deltas into one final
  client-visible `text.output`
- A desktop chat panel that already supports replacing the active assistant entry for a turn

Current implementation shape:

- OpenAI Responses requests are sent with `stream: true`.
- Provider transports incrementally parse SSE chunks.
- Provider deltas are aggregated into one final `std::string` inside `OpenAiLLMs`.
- The gateway protocol still models text output as effectively one final payload per turn.
- The session state currently rejects a second `text.output` for the same active turn.

Target outcome:

- Expose text streaming as a first-class gateway protocol concept instead of a buffered internal
  implementation detail.
- Preserve the application-owned gateway as the only client-facing integration point.
- Keep `turn.completed` as the terminal turn marker.
- Let the client render assistant text incrementally in the chat window as deltas arrive.
- Preserve a clean planner/executor/provider boundary while allowing streamed turn output to flow
  through it.
- Avoid overloading `text.output` with snapshot semantics that conflict with the current contract.

Non-goals for this plan:

- Audio chunk streaming in the first text-streaming slice
- Binary websocket frames for text output
- Replacing the current WebSocket client/server transport
- Multi-turn concurrency within one session
- Multi-step planner execution in the first text-streaming slice
- Realtime API migration for the upstream provider transport

## Recommendation

Implement client-visible text streaming as a protocol-first change, not as repeated reuse of the
existing final `text.output` message.

Recommended contract shape:

- `text.input`
- `text.output.delta`
- `turn.completed`
- `turn.cancel`
- `turn.cancelled`
- `error`

Optional compatibility stance during migration:

- Keep parsing and emitting legacy `text.output` only as a short-lived compatibility bridge inside
  tests and adapter code.
- Make `text.output.delta` the canonical streaming message for all new server/client paths.

Rationale:

- The current protocol and session model encode "one text output per turn". Reusing that message
  for snapshots would work, but it makes the wire contract lie about the semantics.
- Explicit delta messages reduce wire size and make replay/logging/state transitions clearer.
- A protocol-first change keeps provider streaming, gateway orchestration, and client rendering in
  semantic alignment.
- This sets up later audio chunking or richer event streams without another contract rewrite.

Operational interpretation:

- The provider emits deltas.
- The executor boundary remains responsible for step execution and final result capture.
- The responder bridges deltas onto the gateway session transport as deltas, not snapshots.
- The client accumulates deltas into the active assistant transcript entry for the in-flight turn.
- Memory persistence still records the final assistant text only after terminal success.

> [!NOTE]
> **Current status (2026-03-14):**
> - Upstream OpenAI Responses streaming is already implemented behind the provider boundary.
> - The current provider path parses `response.output_text.delta` events incrementally.
> - The current gateway protocol/session layer is still final-output-oriented:
>   - `shared/include/isla/shared/ai_gateway_protocol.hpp`
>   - `shared/src/ai_gateway_session.cpp`
>   - `server/src/ai_gateway_session_handler.cpp`
> - The current responder/executor path buffers provider output before client emission:
>   - `server/src/openai_llms.cpp`
>   - `server/src/ai_gateway_stub_responder.cpp`
> - The current client chat path is already close to streaming-ready:
>   - `client/src/client_app.cpp`
>   - `engine/src/render/model_renderer.cpp`
> - Existing tests already prove that repeated text updates for one turn can replace the active
>   assistant transcript entry in the client, but the shared protocol/session contract does not yet
>   formally permit that behavior.

### Changelog

- 2026-03-14: added a follow-on protocol-first streaming plan for the AI gateway after the Phase
  3.6 OpenAI Responses integration landed with upstream streaming but final-output client
  semantics.

## Architecture Snapshot

Desired steady-state flow:

- `client -> text.input -> gateway`
- `gateway -> streamed provider request -> OpenAI Responses`
- `OpenAI Responses -> text deltas -> gateway`
- `gateway -> text.output.delta frames -> client`
- `gateway -> turn.completed -> client`
- `gateway -> final assistant text -> memory persistence`

Protocol invariant:

- `turn.completed` remains the only successful terminal signal for a text turn.
- `error` remains non-terminal unless followed by `turn.completed` per current accepted-turn error
  policy.
- `text.output.delta` is non-terminal and may appear zero or more times before `turn.completed`.

Session invariant:

- At most one turn remains in flight per session.
- A turn may emit zero or more text deltas.
- A turn may emit zero or one final audio payload in the current non-audio-streaming slice.
- No text deltas may arrive after `turn.completed`, `turn.cancelled`, or accepted-turn terminal
  error handling completes.

## Phase 0: Streaming Protocol Baseline + Contract Update

### Goal

Define the canonical client/gateway streaming contract before changing responder or UI behavior.

### Scope

- Publish the protocol-first streaming shape as the new authoritative direction for gateway text
  turns.
- Decide the canonical message names and minimal required payloads.
- Define migration posture for legacy `text.output`:
  - preferred: retain temporary decode support only where needed for compatibility tests
  - avoid: treating both `text.output` and `text.output.delta` as long-term first-class message
    variants
- Update gateway design documentation references so later implementation phases build on one shared
  contract.
- Document the required ordering rules:
  - `text.input` starts a turn
  - zero or more `text.output.delta`
  - terminal `turn.completed`
  - or terminal `turn.cancelled`
  - or accepted-turn `error` plus `turn.completed`
- Document failure and cancellation interaction with partial output.

### Exit Criteria

- A written protocol-first streaming contract exists in `docs/ai`.
- The contract clearly distinguishes delta, terminal, and error semantics.
- Later implementation phases do not need to redefine event ordering or message ownership.

## Phase 1: Shared Protocol + Session State Migration

### Goal

Teach the shared protocol and session model that streamed text is legal and final-output-only
semantics are no longer the sole path.

### Scope

- Update `shared/include/isla/shared/ai_gateway_protocol.hpp` and
  `shared/src/ai_gateway_protocol.cpp` to add:
  - `MessageType::TextOutputDelta`
  - `TextOutputDeltaMessage { turn_id, delta }`
- Keep wire naming consistent with existing protocol naming conventions.
- Update JSON parse/serialize tests to cover:
  - delta round-trip
  - unknown-type rejection
  - required-field validation
- Update `shared/include/isla/shared/ai_gateway_session.hpp` and
  `shared/src/ai_gateway_session.cpp` to replace the single-final-text assumption with explicit
  streamed-text state.
- Recommended turn-state shape:
  - `text_stream_started`
  - `audio_output_emitted`
  - optional future `text_stream_completed` if later needed for stricter invariants
- Preserve current one-turn-in-flight semantics.
- Preserve current audio-after-text sequencing unless Phase 5 explicitly revisits it.

### Design Notes

- Prefer renaming the current `mark_text_output(...)` behavior to something stream-aware, for
  example:
  - `mark_text_delta(...)`
  - `mark_text_stream_started(...)`
- Do not preserve the current `text output already emitted for active turn` rule.
- Do preserve the rule that no output is valid for a non-active turn.

### Exit Criteria

- Shared protocol code formally supports text deltas.
- Session state no longer rejects repeated streamed text for the active turn.
- Shared tests cover the new legal/illegal state transitions.

## Phase 2: Server Transport + Session Handler Delta Egress

### Goal

Extend the gateway-side typed emission surface so the transport layer can deliver deltas cleanly.

### Scope

- Update `server/src/ai_gateway_session_handler.cpp` and
  `server/include/isla/server/ai_gateway_session_handler.hpp` to add delta emission, for example:
  - `EmitTextOutputDelta(turn_id, delta)`
- Update `server/src/ai_gateway_websocket_session.cpp` and
  `server/include/isla/server/ai_gateway_websocket_session.hpp` so live sessions can emit streamed
  text through the existing queued async write path.
- Extend `GatewayLiveSession` with a delta-specific async emit method instead of reusing
  `AsyncEmitTextOutput(...)`.
- Preserve the single transport boundary for:
  - frame serialization
  - queued writes
  - close/error sequencing
  - logging
- Update integration tests to assert valid frame ordering:
  - one or more `text.output.delta`
  - then `turn.completed`

### Design Notes

- Do not introduce a second websocket session path for streaming.
- Do not bypass `GatewaySessionHandler` when sending deltas.
- Preserve the current callback-based async completion semantics unless there is a concrete need to
  refine them in a later hardening phase.

### Exit Criteria

- The server can legally emit streamed text deltas through the current live-session seam.
- Existing close/error/write-order behavior remains centralized in the same transport adapter.
- Server session-handler and websocket-session tests cover delta egress.

## Phase 3: Executor + Provider Streaming Boundary

### Goal

Stop collapsing provider streaming into one opaque `std::string` before the responder sees it.

### Scope

- Extend `server/include/isla/server/openai_llms.hpp` and `server/src/openai_llms.cpp` so
  `OpenAiLLMs` can surface deltas while still producing a final accumulated `LlmCallResult`.
- Recommended shape:
  - keep `GenerateContent(...)` for final-result callers
  - add a stream-capable variant or callback parameter for text deltas
- Ensure the provider callback path still enforces output budgets incrementally.
- Preserve the current final result object for:
  - memory writes
  - executor result inspection
  - existing final-result-oriented code
- Keep the executor boundary explicit:
  - provider deltas are surfaced through a typed callback or sink
  - final execution result still exists independently of streamed emission

### Design Notes

- Avoid pushing websocket/session concerns down into `OpenAiLLMs`.
- Avoid returning provider transport event types directly from planner/executor APIs.
- Keep the provider-facing callback minimal and gateway-owned, for example:
  - `on_text_delta(std::string_view delta) -> absl::Status`

### Exit Criteria

- Provider deltas can flow through `OpenAiLLMs` without forcing early buffering into one client-only
  final text string.
- Final `LlmCallResult.output_text` still exists for post-turn persistence and diagnostics.
- Provider/executor tests cover delta callback ordering, early abort, and output-budget failures.

## Phase 3.5: Responder Streaming Bridge + Memory Finalization

### Goal

Bridge executor/provider deltas to the live gateway session while keeping final assistant text
ownership clear for memory persistence and terminalization.

### Scope

- Update `server/src/ai_gateway_stub_responder.cpp` so successful turns emit
  `text.output.delta` incrementally while execution is in progress.
- Keep the current accepted-turn terminal rules:
  - success -> `turn.completed`
  - cancellation -> `turn.cancelled`
  - accepted-turn error -> `error` then `turn.completed`
- Accumulate the final assistant text server-side for memory write-back only.
- Re-check cancellation and session liveness between streamed emits.
- Introduce bounded emit coalescing if needed so the client is not sent one websocket frame per
  token under pathological provider chunking.
- Preserve deterministic stop-time terminalization behavior during `server.Stop()`.

### Design Notes

- Coalescing policy belongs here or in a nearby gateway-owned streaming utility, not in the shared
  protocol layer.
- The responder remains the correct place to decide:
  - whether a delta should still be emitted
  - whether the turn has been cancelled
  - whether memory persistence should run
- If coalescing is added, keep it deterministic and testable:
  - by byte threshold
  - by short time window
  - or both

### Exit Criteria

- Successful turns emit streamed text deltas over the live session before terminal completion.
- Final assistant text is still persisted exactly once on successful completion.
- Cancellation, stop-time terminalization, and mid-stream transport loss remain well-defined.

## Phase 4: Client Session + Chat Window Incremental Rendering

### Goal

Render streamed assistant text naturally in the desktop chat window.

### Scope

- Update `client/src/ai_gateway_client_session.cpp` and its tests to parse and surface
  `text.output.delta`.
- Update `client/src/client_app.cpp` so the active assistant transcript entry appends deltas instead
  of replacing full snapshots.
- Preserve the existing one-in-flight-turn UI behavior:
  - the user cannot submit another chat message while a turn is in flight
  - the status line still reflects "Assistant is responding..."
- Preserve transcript ordering:
  - user message first
  - active assistant entry appears once
  - deltas append into that one entry
  - terminal system/error messages still append as separate transcript entries when appropriate
- Keep `engine/src/render/model_renderer.cpp` largely unchanged except for any small polish needed
  to avoid over-aggressive autoscroll churn during streaming.

### Design Notes

- The client should remain dumb about provider semantics; it only knows gateway protocol events.
- The client should not need to reconstruct snapshots from server-specific heuristics.
- Prefer append-based assistant state in `ClientApp` over replacing the entire transcript item every
  time.

### Exit Criteria

- The chat window displays assistant text incrementally for an active turn.
- The final visible transcript for a completed turn matches the accumulated streamed text.
- Client/session/UI tests cover incremental append behavior, transport close mid-stream, and
  accepted-turn error behavior after partial output.

## Phase 5: Hardening, Compatibility Cleanup, and Documentation Close-Out

### Goal

Remove transitional hackiness and leave one clean streaming contract in the repo.

### Scope

- Remove temporary compatibility paths for legacy `text.output` if they were retained during the
  migration.
- Update `docs/ai/ai_gateway_v1_design.md` and `docs/ai/ai-gateway-phased-plan.md` to reflect the
  post-migration text-streaming reality.
- Audit log messages and diagnostics so they refer to deltas/streaming accurately.
- Harden tests around:
  - zero-delta successful turns
  - provider completion without recoverable text
  - provider delta followed by error
  - cancellation between deltas
  - transport closure after partial output
  - server shutdown during streamed output
- Re-evaluate whether current audio-after-text invariants still make sense once text is streamed.
- Document explicit follow-on work that remains deferred:
  - audio chunk streaming
  - richer per-turn stream metadata
  - possible backpressure metrics or observability

### Exit Criteria

- One canonical text-streaming client/gateway contract remains.
- Transitional dual-semantics code paths are removed or explicitly justified.
- The docs match the shipped protocol and architecture.

## Cross-Phase Testing Strategy

1. Keep shared protocol parsing/serialization covered independently of websocket transport tests.
2. Keep session-state legality tests focused on turn lifecycle invariants rather than network timing.
3. Keep websocket/session-adapter tests focused on frame ordering and close/error sequencing.
4. Keep provider/executor tests focused on callback ordering, early abort, and byte-budget
   enforcement.
5. Keep responder tests focused on streamed delta emission, cancellation, memory persistence, and
   terminalization.
6. Keep client/UI tests focused on transcript accumulation and user-visible turn state.
7. Add or preserve end-to-end smoke coverage only after each lower boundary is stable.

## Suggested Delivery Milestones

1. First documented protocol-first streaming baseline: after Phase 0.
2. First legal shared `text.output.delta` protocol/session contract: after Phase 1.
3. First server transport path capable of emitting text deltas: after Phase 2.
4. First provider/executor path that surfaces deltas without collapsing them immediately: after
   Phase 3.
5. First end-to-end streamed text turn through the gateway with final memory persistence: after
   Phase 3.5.
6. First client-visible incremental chat rendering milestone: after Phase 4.
7. First fully cleaned-up streaming contract with docs parity: after Phase 5.
