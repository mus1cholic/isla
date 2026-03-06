# AI Gateway + Voice Pipeline Phased Plan

## Context

`isla` currently has:

- A C++ desktop client/runtime codebase built around Bazel
- No runnable backend/server process yet
- No runnable WebSocket endpoint/server process yet
- No existing OpenAI integration
- No existing text-to-speech integration

Target outcome:

- Add a C++ server that acts as the application-owned AI gateway
- Connect the desktop client to that server over WebSocket
- Route text model requests from the server to OpenAI
- Keep the server transport and orchestration shape ready for chunk-style streaming later
- Add a text-to-speech stage backed by Fish Audio hosted APIs
- Return final text output and optional synthesized audio output over the same gateway protocol
- Preserve a clean planner/executor boundary even though v1 only performs one upstream model request

Non-goals for this plan:

- Audio input/transcription in the initial implementation slice
- Image input/attachments in the initial implementation slice
- OpenAI Realtime API as the first upstream transport
- Self-hosted Fish-Speech GPU workers in the first implementation slice
- Multi-step planner execution in the first implementation slice
- Chunk-based client streaming in the first implementation slice

## Recommendation

Implement the AI pipeline as a C++ gateway server with:

- `client <-> WebSocket <-> gateway server`
- `gateway server <-> HTTP/SSE <-> OpenAI Responses API`
- `gateway server <-> HTTP first, WSS later if needed <-> Fish Audio hosted API`

Rationale:

- A persistent WebSocket is the right fit for the client/server leg because the client will
  eventually need bidirectional streaming semantics, session-oriented turns, and long-lived
  control/event flow.
- OpenAI Responses API over HTTP/SSE keeps the first upstream integration simpler than adopting a
  second websocket protocol immediately.
- A stream-oriented internal adapter boundary preserves future upgrade room for chunk streaming
  without forcing that complexity into the first milestone.
- Fish Audio hosted APIs remove the operational burden of managing GPU workers while the protocol
  and orchestration layers are still being defined.
- A planner/executor split is still worth keeping even when v1 emits exactly one upstream model
  request, because it preserves the long-term orchestration boundary.

Operational interpretation:

- The client only talks to the application-owned gateway.
- The gateway owns protocol validation, session lifecycle, planning, execution, TTS routing, and
  future retries/fallbacks.
- The websocket layer never talks to OpenAI or Fish directly.
- The planner emits exactly one executable upstream model request in v1.
- The executor performs exactly one OpenAI request in v1.
- Fish is downstream of executor output, not a peer of the planner.

Sequencing rule for the remaining work:

- Whole-number phases establish local server boundaries, contracts, or stubbed slices.
- `.5` phases add live external integrations only after the preceding boundary is in place.

> [!NOTE]
> **Current status (2026-03-06):**
> - Phase 0 is implemented.
> - Phase 1 is implemented.
> - The v1 architecture baseline is now published in `docs/ai/ai_gateway_v1_design.md`.
> - Shared protocol/session scaffolding now exists in:
>   - `shared/include/isla/shared/ai_gateway_protocol.hpp`
>   - `shared/include/isla/shared/ai_gateway_session.hpp`
>   - `server/src/ai_gateway_session_handler.hpp`
>   - `server/src/ai_gateway_websocket_session.hpp`
>   - `server/src/ai_gateway_logging_utils.hpp`
> - The implemented Phase-1 slice currently covers:
>   - typed JSON protocol messages and JSON parse/serialize coverage
>   - session/turn lifecycle state enforcement for one in-flight turn per session
>   - a transport-facing session handler that accepts incoming JSON frames and emits protocol
>     frames/events for later WebSocket integration
>   - a WebSocket-facing session adapter that converts text frames to/from the session handler
>   - per-connection session ID generation and factory wiring
>   - transport-boundary connection close/error sequencing for active turns and session teardown
>   - adapter-boundary logging with control-character sanitization for untrusted fields
>   - dedicated protocol/session handler/session-adapter/logging tests under Bazel
> - No runnable gateway server process has been implemented yet.
> - The chosen v1 transport split is:
>   - client/server: WebSocket
>   - server/OpenAI: HTTP/SSE via Responses API
>   - server/Fish: hosted API, HTTP first
> - The chosen v1 orchestration shape is:
>   - one client turn in
>   - one planner output
>   - one executor request to OpenAI
>   - one final text result out
>   - zero or one final audio result out
> - Chunk-style streaming is intentionally deferred at the client protocol level, but the internal
>   server abstraction should remain stream-capable from day one.
> - Audio input and transcription are intentionally deferred from the first server slice.
> - Self-hosted Fish workers, Spot GPU fleets, and OpenAI Realtime transport remain explicitly
>   deferred alternatives rather than current implementation goals.

### Changelog

- 2026-03-06: narrowed the remaining plan to smaller boundary-first slices by splitting live
  integration work into `.5` phases; Phase 2 now stops at runnable gateway ingress/session handoff,
  Phase 2.5 adds a local stubbed return path, Phase 3.5 owns live OpenAI hookup, and Phase 5.5
  owns live Fish hookup.
- 2026-03-06: added initial AI gateway phased plan covering C++ server choice, client/server
  websocket protocol direction, OpenAI Responses API over HTTP/SSE, Fish Audio hosted API
  integration, single-step planner/executor boundary, and deferred streaming/self-hosted expansion.
- 2026-03-06: completed Phase 0 with a dedicated v1 gateway architecture baseline in
  `docs/ai/ai_gateway_v1_design.md`, including the initial WebSocket protocol contract,
  planner/executor boundary, upstream transport decisions, and deferred-alternative inventory.
- 2026-03-06: implemented the core Phase-1 protocol/session scaffolding in `shared/src` and
  `server/src`, including typed JSON message parsing/serialization, session lifecycle state
  enforcement, a transport-facing session handler, and dedicated tests; actual WebSocket adapter
  wiring remains pending.
- 2026-03-06: completed the remaining Phase-1 transport wiring in `server/src` with a
  WebSocket-facing session adapter, sequential per-connection session ID generation, connection
  factory wiring, and transport close/error sequencing tests.
- 2026-03-06: tightened the completed Phase-1 boundary with adapter-level logging, log-sanitization
  utilities for untrusted fields, safer `StatusOr`-based test helpers, and refreshed editor
  compile-command coverage for `server/src`.
- 2026-03-06: reordered the remaining phases so the first runnable gateway server slice now follows
  immediately after Phase 1, without changing the substantive phase content.

## Architecture Snapshot

Authoritative v1 topology:

```text
client <-> WebSocket <-> C++ gateway server
                           |-> HTTP/SSE -> OpenAI Responses API
                           |-> HTTP -> Fish Audio hosted API
```

Planned v1 turn flow:

```text
client text.input
-> gateway websocket/session layer
-> planner
-> execution plan (exactly one upstream model request)
-> executor
-> optional TTS stage
-> gateway response composer
-> client text.output
-> optional client audio.output
-> client turn.completed
```

Future-compatible transport evolution intentionally preserved by this design:

- client/server can later add chunked inbound/outbound events without abandoning WebSocket
- server/OpenAI can later move to a different upstream transport if needed without changing the
  client contract
- server/Fish can later switch from hosted HTTP to hosted WSS or self-hosted workers without
  changing planner or websocket responsibilities

## Phase 0: Gateway Boundary + Protocol Contract

> [!NOTE]
> **Status (2026-03-06): Implemented.**
> - `docs/ai/ai_gateway_v1_design.md` is the canonical Phase 0 architecture baseline.
> - The baseline now freezes:
>   - client/gateway WebSocket ownership and message types
>   - text-input, text-output, optional-audio-output v1 turn semantics
>   - OpenAI Responses API over HTTP/SSE as the first upstream transport
>   - Fish hosted HTTP as the first TTS transport
>   - the single-step planner/executor boundary
>   - the deferred-alternative list for later phases
> - No gateway server code is implemented yet; later phases remain implementation work.

### Goal

Freeze the first server-side architecture and protocol boundaries before implementation begins.

### Scope

- Add a dedicated AI gateway design document and treat it as the v1 architecture baseline.
- Define the first stable client/server transport decision:
  - WebSocket between client and gateway
  - JSON control/event messages
  - text-input turns with optional audio output in the first implementation slice
- Define the first stable upstream transport decision:
  - OpenAI via Responses API over HTTP/SSE
  - Fish via hosted HTTP API in the first implementation slice
- Define the first stable orchestration decision:
  - one planner output per client turn
  - one executor request per planner output
  - one executor request to OpenAI in v1
- Explicitly document deferred alternatives:
  - OpenAI Realtime API
  - client-side chunked audio input
  - self-hosted Fish workers
  - Fish worker Spot GPU fleet
  - multi-step planning

### Exit Criteria

- A written design exists that freezes the initial transport and orchestration shape.
- Later implementation work can reference a single architecture baseline instead of relying on chat
  history.

## Phase 1: Client/Gateway WebSocket Protocol

> [!NOTE]
> **Status (2026-03-06): Implemented.**
> - Implemented so far:
>   - shared protocol types for `session.*`, `text.*`, `audio.output`, `turn.*`, and `error`
>   - JSON parse/serialize support behind the shared protocol boundary
>   - `SessionState` lifecycle enforcement for one active turn per session
>   - `GatewaySessionHandler` for transport-facing frame handling and immediate protocol replies
>   - `GatewayWebSocketSessionAdapter` for WebSocket text-frame/session lifecycle wiring
>   - per-connection session ID generation and `GatewayWebSocketSessionFactory`
>   - transport-boundary close/error sequencing for active turns and session termination
>   - adapter-boundary logging with `SanitizeForLog(...)` for untrusted transport fields
>   - protocol/session/session-handler/session-adapter/logging regression tests

### Goal

Define the first stable client/gateway message contract for text-input turns with text-first and
optional audio output.

### Scope

- Use one WebSocket connection per client session.
- Keep the protocol JSON-only in the first slice.
- Define explicit message `type` values rather than ad hoc payloads.
- Start with a minimal turn-based contract:
  - `session.start`
  - `session.started`
  - `session.end`
  - `session.ended`
  - `text.input`
  - `text.output`
  - `audio.output`
  - `turn.completed`
  - `turn.cancel`
  - `turn.cancelled`
  - `error`
- Require explicit IDs where useful for debugging and future streaming:
  - `session_id`
  - `turn_id`
- Keep at most one in-flight turn per session in v1 so final text/audio delivery order stays
  deterministic.
- Keep the protocol turn-based first instead of chunk-based first.
- Preserve future upgrade room by reserving the ability to add:
  - `text.delta`
  - `audio.output_chunk`
  - `text.input_chunk`
  - `text.input_end`
  - future audio-related event types

### Recommended Initial Payload Shapes

```json
{ "type": "session.start", "client_session_id": "optional-client-id" }
```

```json
{ "type": "session.started", "session_id": "srv_123" }
```

```json
{ "type": "text.input", "turn_id": "turn_1", "text": "Hello" }
```

```json
{ "type": "text.output", "turn_id": "turn_1", "text": "Hi." }
```

```json
{
  "type": "audio.output",
  "turn_id": "turn_1",
  "mime_type": "audio/wav",
  "audio_base64": "UklGRlYAAABXQVZF..."
}
```

```json
{ "type": "turn.completed", "turn_id": "turn_1" }
```

```json
{ "type": "turn.cancel", "turn_id": "turn_1" }
```

```json
{ "type": "turn.cancelled", "turn_id": "turn_1" }
```

```json
{ "type": "session.end", "session_id": "srv_123" }
```

```json
{ "type": "session.ended", "session_id": "srv_123" }
```

```json
{
  "type": "error",
  "session_id": "srv_123",
  "turn_id": "turn_1",
  "code": "bad_request",
  "message": "Missing text"
}
```

### Protocol Invariants

- The server is the authority for runtime session state.
- The server generates `session_id`.
- At most one turn is in flight per session in v1.
- Exactly one `text.output` is produced for each successful `text.input` in v1.
- A successful synthesized turn may additionally produce exactly one `audio.output` before
  `turn.completed`.
- `audio.output` is JSON-carried in v1 using base64 payload data so the initial protocol can remain
  JSON-only.
- `turn.completed` terminates every turn, including error cases.
- `turn.cancelled` also terminates a turn when the server accepts cancellation.
- Errors may omit `turn_id` only when the failure occurs before the server accepts a turn.
- The websocket layer is transport/orchestration plumbing only; it does not build prompts or call
  vendors directly.

### Exit Criteria

- The client/gateway websocket contract is explicit, minimal, and stable enough to implement.
- The contract is exercised by shared/session-handler tests rather than existing only in docs.
- A WebSocket-facing adapter exists that uses the shared protocol/session-handler boundary instead
  of reimplementing the contract ad hoc.
- The protocol can later grow chunked input/output events without requiring a transport redesign.

## Phase 2: Runnable Gateway Ingress + Session Handoff

> [!NOTE]
> **Carry-forward from Phases 0/1 (2026-03-06):**
> - The architecture baseline is documented.
> - The shared protocol/session boundary, transport-facing session handler, WebSocket-facing
>   session adapter, session factory, and log-sanitization utility already exist.
> - This phase intentionally stops at the runnable server/process and application handoff boundary;
>   planner/executor/provider work moves to later phases.

### Goal

Ship the first runnable server slice that accepts real client connections and hands accepted turns
to application-owned code.

### In Scope

- Build the C++ gateway server skeleton.
- Implement the websocket endpoint and first text-input protocol ingress path.
- Reuse the existing shared protocol/session-handler scaffolding instead of re-encoding protocol
  rules directly in the endpoint implementation.
- Add an application-owned handoff boundary for accepted turns and cancel requests.
- Make the server/session lifecycle runnable over a real WebSocket connection.
- Add whatever minimal session registry/controller surface is needed to keep live sessions
  addressable by application-owned code later.

### Out of Scope

- Any application-facing API that emits `text.output`, `audio.output`, `turn.completed`,
  `turn.cancelled`, or `error` back to the client.
- Any local stub/echo responder for accepted turns.
- Planner or executor contracts.
- OpenAI config, provider wiring, or network traffic.
- Fish/TTS config, provider wiring, or network traffic.
- Any threading/background-work design that is not required by the concrete websocket server
  runtime itself.

### Required Artifacts

- A runnable gateway server process with a real websocket listener.
- A live websocket/session integration that creates `GatewayWebSocketSessionAdapter`-backed
  sessions for real connections.
- An application-owned accepted-turn/cancel handoff seam that receives typed events rather than raw
  client JSON.
- A session ownership model that makes later server-owned emission possible without moving protocol
  parsing or frame ordering out of the transport boundary.

### Recommended Delivery Order

1. Gateway server process and config surface
2. WebSocket endpoint and session lifecycle
3. Session registry/controller around live adapters
4. `text.input`/`turn.cancel` handoff into application-owned code

### Demo

- A real websocket client can connect, complete `session.start`, send `text.input`, and cause the
  server to record or forward a typed accepted-turn event.
- The same client can send `turn.cancel` for an accepted turn and cause the server to record or
  forward a typed cancel-request event.

### Tests

- Keep protocol/session validation covered at the existing unit-test boundary.
- Add at least one integration-style test that uses a real websocket connection to a runnable
  server process while substituting a fake application sink.
- Verify that no application layer outside the transport boundary needs to parse raw client JSON to
  observe accepted turns or cancel requests.

### Exit Criteria

- A client can connect to the gateway over WebSocket and complete `session.start`/`session.end`
  against a runnable server process.
- A client `text.input` can reach an application-owned accepted-turn handoff over a real socket
  without re-parsing raw client JSON outside the transport boundary.
- Turn-cancel requests can reach the same application-owned handoff boundary.
- The server structure is ready for later outbound turn completion work without changing transport
  ownership.

## Phase 2.5: Local Stub Turn Completion Path

### Goal

Close the loop through the same session boundary without introducing provider dependencies yet.

### In Scope

- Add an application-facing path for server-owned code to emit `text.output`, `turn.completed`,
  `turn.cancelled`, and `error` back through live sessions.
- Implement a deterministic local stub or echo responder for accepted text turns.
- Exercise final frame ordering through the existing session/adapter boundary rather than writing
  frames directly in new code.
- Prove that accepted-turn ingress and outbound turn completion can both flow through the same live
  session ownership model.

### Out of Scope

- Audio output.
- Planner or executor contracts.
- OpenAI config, provider wiring, or network traffic.
- Fish/TTS config, provider wiring, or network traffic.
- Any provider-specific retry, fallback, or streaming policy.

### Required Artifacts

- An application-facing emission API or handle that can target a live accepted turn without
  exposing raw websocket frame construction to the caller.
- A deterministic stubbed turn responder that returns final text through that emission path.
- Explicit turn-finalization behavior for both successful stubbed turns and accepted cancellations.

### Demo

- A real websocket client can connect to the runnable gateway, send a text query, and receive a
  final deterministic non-provider `text.output` followed by `turn.completed`.
- A real websocket client can cancel an accepted turn and observe the turn terminate through the
  same server-owned boundary.

### Tests

- Add integration-style coverage that exercises the live server/socket path with the stubbed
  responder enabled.
- Verify final frame ordering for successful stubbed turns.
- Verify cancellation termination behavior through the same application-facing emission seam.

### Exit Criteria

- A client can connect to the runnable gateway, send a text query, and receive a final non-provider
  text response plus `turn.completed`.
- A cancelled accepted turn can terminate through the same server-owned session boundary.
- The server now has explicit application-facing ingress and egress seams for later planner,
  executor, and provider work.

## Phase 3: OpenAI Executor Boundary

> [!NOTE]
> **Carry-forward from Phase 1 (2026-03-06):**
> - Shared protocol types, session state enforcement, and a transport-facing session handler now
>   exist.
> - A WebSocket-facing session adapter, session factory, and log-sanitization utility now exist in
>   `server/src`.
> - Phase 3 should build on those boundaries rather than redefining client/gateway message
>   parsing, turn lifecycle rules, or transport logging rules inside executor code.
> - Live OpenAI network integration is intentionally deferred to Phase 3.5.

### Goal

Define the stable executor-side boundary for one OpenAI-backed request without yet depending on
live provider I/O.

### Scope

- Define normalized executor request/result/error shapes for exactly one upstream text request in
  v1.
- Keep the executor boundary stream-capable internally even if the first product behavior remains
  final-output-oriented.
- Separate provider I/O concerns from websocket/session ownership and planner policy.
- Define the config/error surface that a later OpenAI adapter must satisfy.
- Defer actual OpenAI Responses API HTTP/SSE traffic to Phase 3.5.

### Intended Behavior

- The executor contract still models one OpenAI request per turn in v1.
- The executor boundary may internally model streamed upstream events.
- The gateway may buffer upstream chunks and emit only the final text to the client in v1.
- Future partial output can be added later without changing planner or client transport ownership.

### Deferred Alternatives

- live OpenAI provider traffic in this phase
- OpenAI Realtime API
- OpenAI upstream websocket transport
- exposing partial output deltas to the client in the first milestone

### Exit Criteria

- The server has a clear OpenAI executor boundary independent of websocket handling.
- The design preserves future streaming support without requiring v1 to expose deltas.
- The executor contract is explicit enough to wire to a fake or stub provider before live OpenAI
  integration exists.

## Phase 3.5: OpenAI Responses Integration

### Goal

Route gateway text requests to OpenAI through the executor boundary established in Phase 3.

### Scope

- Integrate OpenAI through the Responses API first, not Realtime API.
- Treat the OpenAI leg as streamed HTTP/SSE rather than WebSocket in the first implementation.
- Map provider responses and provider-originated errors into the gateway-owned executor interface.
- Buffer streamed upstream text into one final client-visible `text.output` in v1.
- Keep provider-originated details routed through the existing transport boundary so later logging
  remains sanitized and centralized.

### Exit Criteria

- The gateway can produce a final OpenAI-backed text response through the executor boundary.
- Live OpenAI integration does not leak provider transport details into websocket/session code.

## Phase 4: Planner/Executor Single-Step Orchestration

> [!NOTE]
> **Carry-forward from Phases 2/2.5/3.5 (2026-03-06):**
> - The runnable gateway/server ingress path and application-owned session handoff boundary should
>   already exist.
> - Planner/executor work should consume accepted-turn/cancel events from that boundary rather than
>   reinterpreting raw client JSON.
> - The live OpenAI path should already sit behind the executor boundary rather than being called
>   directly from websocket code.

### Goal

Lock down the first planner/executor orchestration contract around the working single-step path.

### Scope

- Define the planner as a normalizer that turns a client turn into exactly one executable model
  request in v1.
- Define the executor as the module that performs exactly one OpenAI request in v1.
- Keep planner output structured rather than implicit.
- Keep TTS decisions downstream of the planner/executor flow.
- Explicitly document that multi-step plans are deferred, not absent from the long-term design.

### Suggested Data Contracts

```cpp
struct ClientTurnInput {
  std::string session_id;
  std::string turn_id;
  std::string user_text;
};

struct PlannedRequest {
  std::string session_id;
  std::string turn_id;
  std::string system_prompt;
  std::string user_text;
  std::string model;
  bool should_synthesize;
};

struct ExecutorResult {
  std::string session_id;
  std::string turn_id;
  std::string model;
  std::string output_text;
};

struct SynthesizedAudio {
  std::string mime_type;
  std::vector<uint8_t> bytes;
};

struct TurnResult {
  std::string session_id;
  std::string turn_id;
  std::string output_text;
  std::optional<SynthesizedAudio> audio;
};
```

### Orchestration Invariants

- Exactly one planner output per turn in v1.
- Exactly one OpenAI request per planner output in v1.
- Planner owns request-shaping policy.
- Executor owns provider I/O.
- TTS consumes executor output; it does not alter planner/executor boundaries.

### Exit Criteria

- The planner/executor contract is explicit enough to implement without embedding policy into the
  websocket layer.
- The design preserves room for future multi-request planning without forcing it into v1.

## Phase 5: TTS Adapter Boundary

> [!NOTE]
> **Carry-forward from Phase 1 (2026-03-06):**
> - The existing WebSocket/session layer already owns final frame emission ordering.
> - Fish integration should produce normalized audio results for that layer rather than writing
>   websocket frames or logs directly.

### Goal

Define the server-owned TTS boundary without taking on live vendor integration yet.

### Scope

- Treat the gateway-to-TTS boundary as a narrow adapter rather than a platform-wide dependency.
- Keep the adapter stream-capable internally even if the first implementation only returns a final
  audio result.
- Keep TTS downstream of executor text output.
- Define the normalized audio result shape expected by the gateway response composer.
- Preserve the v1 client behavior of one final `audio.output` event with metadata and inline base64
  payload.
- Defer actual Fish hosted integration to Phase 5.5.

### Recommended Gateway/Fish Shape

```text
gateway text result
-> TTS adapter
-> Fish hosted HTTP API
-> final audio result
-> gateway response composer
-> client audio.output
```

### Why Hosted First

- No GPU fleet or Spot interruption handling is required while the protocol is still being defined.
- The gateway remains the application-owned system boundary.
- Hosted TTS can be replaced later by hosted websocket streaming or self-hosted workers without
  changing client/server transport.

### Deferred Alternatives

- live Fish integration in this phase
- Fish Audio hosted websocket streaming in the first implementation
- websocket binary frames for v1 audio delivery
- out-of-band signed audio URLs for v1 delivery
- self-hosted Fish-Speech workers
- AWS Spot GPU worker pools
- load balancer- or fleet-based worker routing

### Exit Criteria

- The design contains a clear TTS adapter boundary that does not leak vendor transport details into
  planner, executor, or websocket code.
- Hosted Fish remains the intended v1 TTS backend, but no live Fish dependency is required yet.

## Phase 5.5: Fish Audio Hosted TTS Integration

### Goal

Add the first live text-to-speech stage through the TTS adapter boundary without taking on
self-hosted GPU infrastructure.

### Scope

- Use Fish Audio hosted APIs rather than self-hosted Fish-Speech workers in the first
  implementation slice.
- Start with simple request/response HTTP integration.
- Keep Fish integration downstream of executor text output.
- Deliver synthesized audio to the client as one final `audio.output` event with metadata and
  inline base64 payload in v1.

### Exit Criteria

- The gateway can optionally emit final synthesized audio through the established TTS boundary.
- Live Fish integration does not leak vendor transport details into planner, executor, or websocket
  code.

## Phase 6: Streaming-Ready Internal Event Model

> [!NOTE]
> **Carry-forward from Phase 1 (2026-03-06):**
> - Phase-1 protocol code already reserves `text.delta` and `audio.output_chunk` event names.
> - Later streaming work should extend the existing shared protocol/session-handler boundary rather
>   than introducing a second parallel client contract.
> - The current adapter already centralizes frame writes, close sequencing, and log sanitization;
>   streaming extensions should preserve that single transport boundary.

### Goal

Preserve future chunk-style streaming capability without exposing that complexity to the client
immediately.

### Scope

- Define internal provider adapters as stream-capable from day one.
- Model one logical event stream per turn rather than one giant opaque blocking call.
- Allow provider adapters to emit events such as:
  - `started`
  - `delta`
  - `completed`
  - `error`
- Allow the Fish adapter to later emit:
  - `started`
  - `audio_chunk`
  - `completed`
  - `error`
- Buffer and coalesce provider events in v1 so the client still sees a simpler final-output
  protocol.
- Preserve the ability to later expose chunked websocket events without changing the transport
  choice.
- Preserve the ability to later replace inline `audio.output` payloads with chunked or binary audio
  events without changing turn/session ownership.

### Design Interpretation

- Do not ignore streaming at the abstraction level.
- Do ignore or buffer it at the client-product surface in the first milestone.
- Keep the server as the place where protocol simplification happens.

### Exit Criteria

- The server design can later surface upstream deltas or chunked TTS output without a major
  refactor.
- The initial client product behavior remains intentionally simple.

## Phase 7: Voice Expansion + Deferred Infrastructure Revisit

### Goal

Revisit the deferred voice/streaming/infrastructure options after the first gateway slice works.

### Scope

- Evaluate whether to expose `text.delta` to the client.
- Evaluate whether to upgrade Fish integration from hosted HTTP to hosted websocket streaming.
- Evaluate whether audio input/transcription should be added.
- Revisit the self-hosted Fish worker alternative only after the gateway protocol and TTS adapter
  stabilize.
- Revisit Spot GPU worker pools only after real usage/cost/latency data exists.
- Revisit OpenAI upstream transport choices only after a concrete need appears.

### Explicitly Deferred Until This Phase

- client-side chunked input
- client-visible partial output
- OpenAI upstream websocket transport
- Fish self-hosted workers
- Fish Spot GPU fleet
- multi-step planner execution

### Exit Criteria

- Deferred alternatives are evaluated against working gateway behavior rather than speculation.
- Any infrastructure escalation beyond hosted Fish is justified by measured needs.

## Cross-Phase Testing Strategy

1. Keep websocket protocol parsing/validation testable outside full provider integration.
2. Keep websocket/session adapter behavior and log-sanitization testable without a real socket
   server.
3. Keep planner and executor testable as independent modules.
4. Keep provider adapters behind interfaces so they can be faked in tests.
5. Prefer structural assertions over brittle transport timing assumptions.
6. Add end-to-end smoke coverage only after module boundaries are stable.

## Suggested Delivery Milestones

1. First documented architecture baseline: after Phase 0.
2. First stable websocket text-turn protocol: after Phase 1.
3. First runnable gateway ingress/session-handoff milestone: after Phase 2.
4. First local stubbed turn-completion path through the gateway: after Phase 2.5.
5. First explicit OpenAI executor boundary: after Phase 3.
6. First OpenAI-backed text response through the gateway: after Phase 3.5.
7. First explicit TTS adapter boundary: after Phase 5.
8. First hosted TTS-backed end-to-end response path: after Phase 5.5.
9. First streaming-ready internal event model without client-visible deltas: after Phase 6.
10. First evidence-based revisit of streaming/self-hosted alternatives: after Phase 7.
