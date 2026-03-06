# AI Gateway v1 Design Baseline (Phase 0)

Last updated: 2026-03-06

## Purpose

This document freezes the initial `isla` AI gateway architecture and protocol boundaries for the
first implementation slice.

It is the authoritative Phase 0 baseline for:

- client/gateway transport ownership
- gateway/provider transport ownership
- v1 turn orchestration boundaries
- the initial WebSocket message contract
- deferred alternatives that are intentionally out of scope for v1

Later implementation phases should extend this document instead of redefining the baseline in chat
or code comments.

## Current Repository Status

As of 2026-03-06:

- the v1 architecture baseline is documented
- shared protocol/session scaffolding exists in:
  - `shared/include/isla/shared/ai_gateway_protocol.hpp`
  - `shared/include/isla/shared/ai_gateway_session.hpp`
  - `server/src/ai_gateway_session_handler.hpp`
  - `server/src/ai_gateway_websocket_session.hpp`
- the repo now has:
  - typed protocol message definitions
  - JSON parse/serialize support for the v1 message contract
  - session/turn lifecycle enforcement for one in-flight turn per session
  - a transport-facing session handler that consumes incoming JSON frames and emits outgoing
    protocol frames/events
  - a WebSocket-facing session adapter and session factory that wire per-connection session IDs,
    text-frame handling, and transport close/error sequencing
- no runnable AI gateway server process exists yet
- no OpenAI integration exists yet
- no Fish Audio integration exists yet

This document defines the architecture baseline that later code should implement. The current
Phase-1 code is a partial realization of this contract, not a complete gateway server.

## Normative Terms

- MUST: required for v1 compatibility
- SHOULD: recommended unless a concrete reason exists to differ
- MAY: optional

## v1 Topology

Authoritative v1 topology:

```text
desktop client <-> WebSocket <-> C++ gateway server
                                  |-> HTTP/SSE -> OpenAI Responses API
                                  |-> HTTP -> Fish Audio hosted API
```

Ownership rules:

- The desktop client MUST talk only to the application-owned gateway.
- The gateway MUST own session lifecycle, protocol validation, planning, execution, TTS routing,
  and response composition.
- The client MUST NOT talk directly to OpenAI or Fish Audio.
- The WebSocket layer MUST NOT call vendors directly; it is transport/orchestration plumbing.

## v1 Product Shape

The initial implementation slice is intentionally narrow:

- input: text only
- output: final text always, optional final synthesized audio
- one accepted client turn maps to one planner output
- one planner output maps to one OpenAI request
- one turn is in flight per session

Explicit non-goals for v1:

- audio input or transcription
- image input or attachments
- client-visible partial text deltas
- chunked client audio delivery
- OpenAI Realtime as the first upstream transport
- self-hosted Fish workers
- multi-step planning

## Turn Flow

Authoritative v1 turn flow:

```text
client text.input
-> gateway session layer
-> planner
-> planned request
-> executor
-> final text result
-> optional TTS adapter
-> response composer
-> client text.output
-> optional client audio.output
-> client turn.completed
```

Turn flow rules:

- The gateway MUST emit exactly one `text.output` for each successful accepted turn.
- The gateway MAY emit exactly one `audio.output` for a turn when synthesis is requested and
  succeeds.
- The gateway MUST emit `turn.completed` after all successful turn outputs for that turn.
- If a turn fails after acceptance, the gateway MUST emit `error` and then terminate the turn with
  `turn.completed` or `turn.cancelled`.

## Client/Gateway Transport Contract

The client/gateway leg MUST use one WebSocket connection per live session.

The v1 protocol MUST remain JSON-only.

The server is authoritative for runtime session state:

- the server MUST generate `session_id`
- the client MUST provide `turn_id` on each `text.input`
- the gateway MUST allow at most one in-flight turn per session in v1

### Required Message Types

The initial protocol baseline consists of:

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

Current implementation note (2026-03-06):

- message structs and JSON parse/serialize support are implemented
- session state enforcement is implemented
- a transport-facing session handler is implemented
- a WebSocket-facing session adapter/factory is implemented for text-frame handling and
  connection-lifecycle wiring

### Message Shapes

Session open:

```json
{ "type": "session.start", "client_session_id": "optional-client-id" }
```

```json
{ "type": "session.started", "session_id": "srv_123" }
```

Turn input:

```json
{ "type": "text.input", "turn_id": "turn_1", "text": "Hello" }
```

Final text output:

```json
{ "type": "text.output", "turn_id": "turn_1", "text": "Hi." }
```

Optional final audio output:

```json
{
  "type": "audio.output",
  "turn_id": "turn_1",
  "mime_type": "audio/wav",
  "audio_base64": "UklGRlYAAABXQVZF..."
}
```

Turn termination:

```json
{ "type": "turn.completed", "turn_id": "turn_1" }
```

Turn cancellation:

```json
{ "type": "turn.cancel", "turn_id": "turn_1" }
```

```json
{ "type": "turn.cancelled", "turn_id": "turn_1" }
```

Session close:

```json
{ "type": "session.end", "session_id": "srv_123" }
```

```json
{ "type": "session.ended", "session_id": "srv_123" }
```

Error:

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

- `audio.output` MUST be emitted only after `text.output` for the same successful turn in v1.
- `audio.output` MUST carry a self-describing `mime_type`.
- `audio.output` MUST carry inline base64 payload data in v1 so the protocol stays JSON-only.
- `turn.completed` MUST terminate every successful or failed turn that was not cancelled.
- `turn.cancelled` MUST terminate a turn after accepted cancellation.
- `error` MAY omit `turn_id` only for failures that occur before a turn is accepted.

### Reserved Future Events

The protocol MUST preserve room for later additions without changing the transport choice:

- `text.delta`
- `audio.output_chunk`
- `text.input_chunk`
- `text.input_end`
- future audio-input event types

## Upstream Provider Contract

### OpenAI

The gateway MUST integrate with OpenAI through the Responses API over HTTP/SSE in v1.

Rules:

- the executor MUST own OpenAI I/O
- the WebSocket layer MUST NOT own OpenAI I/O
- the gateway MAY consume streamed upstream events internally
- the gateway SHOULD buffer and coalesce upstream text into one final client-visible `text.output`
  in v1

Deferred alternatives:

- OpenAI Realtime API
- upstream WebSocket transport to OpenAI
- client-visible partial text deltas in v1

### Fish Audio

The gateway MUST integrate with Fish Audio hosted APIs over HTTP in v1.

Rules:

- TTS MUST sit downstream of executor text output
- the TTS adapter MUST remain a narrow vendor boundary
- the gateway MUST deliver synthesized audio to the client as one final `audio.output` event in v1

Deferred alternatives:

- Fish hosted WebSocket streaming
- WebSocket binary frames for v1 audio delivery
- out-of-band signed audio URLs for v1 delivery
- self-hosted Fish workers
- GPU fleet or Spot orchestration

## Planner/Executor Boundary

The planner/executor split is required in v1 even though only one upstream request is performed.

Responsibilities:

- planner: normalize a client turn into exactly one executable request
- executor: perform exactly one OpenAI request and return normalized provider output
- TTS adapter: optionally synthesize audio from executor text output

Illustrative contract:

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

Invariants:

- exactly one planner output per accepted turn in v1
- exactly one OpenAI request per planner output in v1
- planner owns request shaping and model-selection policy
- executor owns provider I/O
- TTS MUST NOT change planner or executor ownership boundaries

## Streaming Readiness

The implementation MUST preserve a stream-capable internal model even though v1 client behavior is
final-output-oriented.

Required interpretation:

- provider adapters SHOULD model logical event streams internally
- the gateway MAY buffer provider deltas and return only final results in v1
- later chunked text or audio events MUST be addable without changing session or turn ownership

## Phase 0 Acceptance

Phase 0 is complete when:

- this document exists and is treated as the v1 baseline
- the phased plan references this document as the architecture baseline
- deferred alternatives are explicitly documented here instead of left implicit in chat history

## Phase 1 Carry-Forward

As of 2026-03-06, the following Phase-1-aligned implementation exists:

- shared protocol types and JSON parsing/serialization
- shared session lifecycle state enforcement
- a transport-facing session handler that returns:
  - outgoing protocol frames
  - accepted-turn events
  - cancel-request events
- a WebSocket-facing session adapter that turns text frames into handler calls, writes returned
  frames, and owns connection close/error sequencing
- per-connection session ID generation via `GatewayWebSocketSessionFactory`
