#pragma once

// ai_gateway_stub_responder.hpp
//
// GatewayStubResponder is the concrete GatewayApplicationEventSink that glues
// the gateway transport layer to the rest of the server: per-session memory
// orchestrators, the LLM plan executor, retrieval/rerank clients, and the
// sleep-cycle boundary. Despite the "stub" name it is the production responder
// today — the name reflects its origin as a minimal-loop test scaffold that
// has since grown into the real application layer.
//
// Responsibilities:
//   * Owns a worker thread pool and a per-session Asio strand so turns from a
//     single session serialize deterministically while distinct sessions run
//     concurrently.
//   * Maintains a MemoryOrchestrator per live session and funnels user/
//     assistant messages through it, producing rendered system + working-
//     memory prompts for each turn.
//   * Executes accepted turns end-to-end via GatewayPlanExecutor (LLM call,
//     tool invocations, streaming text output) and reports the terminal state
//     back to the transport via the GatewayLiveSession async-emit API.
//   * Exposes synchronous helpers (RenderSessionMemoryPrompt,
//     SnapshotSessionWorkingMemoryState, AwaitSessionMemorySettled,
//     RunSessionSleepCycle, RunAcceptedTurnToCompletion, WaitForAcceptedTurns)
//     that tests and benchmarks use to drive the responder without a real
//     WebSocket client.
//
// All public methods are thread-safe unless noted otherwise; the transport
// layer invokes the GatewayApplicationEventSink overrides from the session's
// I/O strand, and test helpers are typically called from the main thread.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_executor.hpp"
#include "isla/server/ai_gateway_llm_runtime_config.hpp"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/embedding_client.hpp"
#include "isla/server/gemini_api_embedding_client.hpp"
#include "isla/server/jina_api_reranker_client.hpp"
#include "isla/server/llm_client.hpp"
#include "isla/server/memory/memory_orchestrator.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/openai_responses_client.hpp"
#include "isla/server/reranker_client.hpp"

namespace isla::server::ai_gateway {

// Default LLM model used by the mid-term flush decider when the caller does
// not override it via GatewayLlmRuntimeConfig.
inline constexpr std::string_view kDefaultMidTermMemoryModel = kDefaultMidTermFlushDeciderModel;
// Default hard cap on the rendered system-prompt section of a turn's prompt.
// Exceeding the cap logs an ERROR and fails the turn (the responder rejects
// the rendered prompt rather than silently truncating) so a pathological
// working-memory snapshot cannot quietly blow through the provider's token
// budget.
inline constexpr std::size_t kDefaultMaxRenderedSystemPromptBytes =
    static_cast<std::size_t>(64U * 1024U);
// Default hard cap on the rendered working-memory context section of a turn's
// prompt. Independent from the system-prompt cap so retrieval results can be
// sized separately from static instructions. Enforcement is identical: the
// turn is rejected rather than truncated when the limit is exceeded.
inline constexpr std::size_t kDefaultMaxRenderedWorkingMemoryContextBytes =
    static_cast<std::size_t>(64U * 1024U);

// Resolves session-scoped timestamps used by the responder's memory-facing replay path.
//
// Production calls may leave all methods unresolved and fall back to wall-clock time. Eval replay
// can provide deterministic chronology through one shared session-clock object instead of patching
// individual timestamp call sites.
class GatewaySessionClock {
  public:
    virtual ~GatewaySessionClock() = default;

    // Returns the wall-clock timestamp at which `session_id` should be
    // considered to have started. Production clocks return nullopt so the
    // responder falls back to the current wall-clock time; eval replay clocks
    // return the recorded session start so downstream memory writes line up
    // with the replay transcript.
    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveSessionStartTime(std::string_view session_id) const {
        static_cast<void>(session_id);
        return std::nullopt;
    }

    // Returns the wall-clock timestamp for a specific user or assistant
    // message within a turn. Same production-vs-replay contract as
    // ResolveSessionStartTime: nullopt means "use wall-clock now".
    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveConversationMessageTime(std::string_view session_id, std::string_view turn_id,
                                   isla::server::memory::MessageRole role) const {
        static_cast<void>(session_id);
        static_cast<void>(turn_id);
        static_cast<void>(role);
        return std::nullopt;
    }

    // Returns the "reference now" used by memory decay and activeness
    // computations. Production returns nullopt (falls back to wall-clock now);
    // eval replay pins this to the recorded transcript time so decay curves
    // are deterministic across replay runs.
    [[nodiscard]] virtual std::optional<isla::server::memory::Timestamp>
    ResolveEvaluationReferenceTime(std::string_view session_id) const {
        static_cast<void>(session_id);
        return std::nullopt;
    }
};

// Tunables and injected dependencies for GatewayStubResponder. Default-
// constructed configs yield a minimal in-memory responder with no persistence,
// no LLM, and stub timing — suitable for unit tests. Production callers
// populate the memory_store / llm_client / openai_client / embedding_client /
// reranker_client fields to wire in real backends.
struct GatewayStubResponderConfig {
    // Per-turn scheduling delay applied unconditionally in OnTurnAccepted
    // before the worker picks up the turn — it opens a cancellation window
    // between accept and execution and gives tests a knob for simulating
    // provider latency. Applies to every accepted turn regardless of
    // whether a real LLM client is configured.
    std::chrono::milliseconds response_delay{ 50 };
    // Upper bound on any single async emit back to the transport layer. If a
    // GatewayLiveSession async-emit callback does not fire within this window
    // the responder logs and treats the emit as failed so it cannot block the
    // worker thread indefinitely on a wedged transport.
    std::chrono::milliseconds async_emit_timeout{ std::chrono::seconds(2) };
    // Number of shared worker threads used for session startup and accepted-turn execution.
    // A value of 0 uses an implementation-defined default.
    std::size_t worker_pool_size = 0;
    // Total number of attempts to persist a session on startup. A value of 0 is
    // treated as 1. At least one attempt is always made if a memory store is configured.
    std::size_t session_start_persistence_max_attempts = 3;
    // Fixed delay between session-start persistence retries. No exponential
    // backoff — session start is on the hot path for a connecting client and
    // the retry budget is small by design.
    std::chrono::milliseconds session_start_persistence_retry_delay{ 100 };
    // The flush decider runs after an assistant reply only when at least this many new user turns
    // have accumulated since the last decider run.
    std::size_t mid_term_flush_decider_interval_user_turns =
        isla::server::memory::kDefaultMidTermFlushDeciderIntervalUserTurns;
    // Persistent memory backend. When null, the responder runs in fully
    // in-memory mode: sessions are created without durable state, sleep
    // cycles become no-ops, and all memory reads/writes stay process-local.
    isla::server::memory::MemoryStorePtr memory_store;
    // Per-turn LLM runtime parameters (model id, temperature, max tokens, tool
    // catalogue). Shared across all turns produced by this responder.
    GatewayLlmRuntimeConfig llm_runtime_config;
    // Primary chat LLM client. When null the responder falls back to stubbed
    // canned replies paced by `response_delay`.
    std::shared_ptr<const isla::server::LlmClient> llm_client;
    // Config for an optional OpenAI Responses API client used for the planner/
    // tool-calling execution path. Only consulted when `openai_client` is null
    // and the responder needs to construct one internally.
    OpenAiResponsesClientConfig openai_config;
    // Pre-constructed OpenAI Responses client. When provided, takes precedence
    // over `openai_config` and is shared directly with the plan executor.
    std::shared_ptr<const OpenAiResponsesClient> openai_client;
    // Config for an optional Gemini embedding client used by the retrieval
    // path and sleep-cycle edge embedding. Only consulted when
    // `embedding_client` is null.
    GeminiApiEmbeddingClientConfig gemini_api_embedding_config;
    // Pre-constructed embedding client. Shared between read-path similarity
    // search and write-path relationship-edge embedding; see
    // embedding_client.hpp for the error-handling contract.
    std::shared_ptr<const isla::server::EmbeddingClient> embedding_client;
    // Config for an optional Jina cross-encoder reranker used to rescore the
    // retrieval candidate set. Only consulted when `reranker_client` is null.
    JinaApiRerankerClientConfig jina_api_reranker_config;
    // Pre-constructed reranker client. When null, the retrieval pipeline
    // skips the rerank pass and uses first-pass ordering directly.
    std::shared_ptr<const isla::server::RerankerClient> reranker_client;
    // Candidates whose reranker score falls below this threshold are dropped
    // from the retrieval set before prompt rendering.
    double retrieved_memory_reranker_min_score =
        isla::server::memory::kDefaultRetrievedMemoryRerankerMinScore;
    // Per-turn cap on the rendered system-prompt section (see
    // kDefaultMaxRenderedSystemPromptBytes).
    std::size_t max_rendered_system_prompt_bytes = kDefaultMaxRenderedSystemPromptBytes;
    // Per-turn cap on the rendered working-memory context section (see
    // kDefaultMaxRenderedWorkingMemoryContextBytes).
    std::size_t max_rendered_working_memory_context_bytes =
        kDefaultMaxRenderedWorkingMemoryContextBytes;
    // Overall per-turn prompt byte budget. Defaults to the sum of the
    // system-prompt and working-memory budgets when unset; set explicitly
    // to impose a tighter combined cap.
    std::optional<std::size_t> max_rendered_prompt_bytes;
    // Test hook fired whenever the plan executor produces a new ExecutionPlan
    // for a turn. Production leaves this unset; tests use it to assert on
    // planner behaviour without reaching into the executor.
    std::function<void(const ExecutionPlan&)> on_execution_plan;
    // Optional session-scoped clock for deterministic eval replay. When null,
    // all timestamp resolution falls through to wall-clock time.
    std::shared_ptr<const GatewaySessionClock> session_clock;
    // Test hook fired as soon as a turn's retrieval result is ready (before
    // the LLM call). Used by eval harnesses to assert on retrieved memory
    // content without having to serialize through the reply text.
    std::function<void(std::string_view, const isla::server::memory::UserQueryMemoryResult&)>
        on_user_query_memory_ready;
};

// Terminal disposition of an accepted turn after RunAcceptedTurnToCompletion
// (or the async worker loop) has driven it to completion. Mirrors the
// turn.completed / turn.cancelled / error frames emitted on the wire.
enum class GatewayAcceptedTurnTerminalState {
    kSucceeded,
    kFailed,
    kCancelled,
};

// Structured failure payload attached to a turn that terminated in kFailed.
// `code` matches the wire-level error code emitted to the client; `message`
// is a human-readable detail suitable for logging.
struct GatewayAcceptedTurnFailure {
    std::string code;
    std::string message;
};

// Aggregated result returned by RunAcceptedTurnToCompletion. `reply_text` is
// populated on kSucceeded (the final assistant text handed back to the
// client); `failure` is populated on kFailed; both are nullopt on kCancelled.
struct GatewayAcceptedTurnResult {
    GatewayAcceptedTurnTerminalState state = GatewayAcceptedTurnTerminalState::kFailed;
    std::optional<std::string> reply_text;
    std::optional<GatewayAcceptedTurnFailure> failure;
};

// Production GatewayApplicationEventSink implementation. See the file-level
// header for the overall role. The class is thread-safe: event-sink overrides
// can be called from the gateway I/O thread, test helpers from the main
// thread, and the internal worker thread handles accepted turns concurrently.
// Destruction stops the worker pool and drains any in-flight turns before
// returning — do not destroy while the owning GatewayServer is still running.
class GatewayStubResponder final : public GatewayApplicationEventSink {
  public:
    // Constructs a responder with the given config. The constructor eagerly
    // initializes mid-term memory components (flush decider, compactor,
    // sleep-cycle extractor) from the injected clients; failures there are
    // captured and exposed via MidTermMemoryInitializationStatus() so callers
    // can detect them programmatically. The retrieved-memory reranker is
    // also constructed and warmed up eagerly, but any reranker creation or
    // warmup failure is logged only and does NOT flow into
    // MidTermMemoryInitializationStatus() — callers that need to hard-fail
    // startup on a bad reranker must validate the client themselves before
    // handing it in. The LLM and embedding clients themselves are not
    // invoked during construction.
    explicit GatewayStubResponder(GatewayStubResponderConfig config = {});
    // Stops the worker, joins the thread pool, and tears down all per-session
    // state. Blocks until every in-flight turn either finishes or hits
    // `async_emit_timeout`.
    ~GatewayStubResponder() override;

    GatewayStubResponder(const GatewayStubResponder&) = delete;
    GatewayStubResponder& operator=(const GatewayStubResponder&) = delete;

    // Wires the responder to the GatewayServer's session registry so it can
    // look up live sessions by id for out-of-band emits (error frames, turn
    // outputs from the worker thread). Must be called before Start() on the
    // owning server — typically immediately after both have been constructed.
    void AttachSessionRegistry(GatewaySessionRegistry* session_registry);

    // Synchronous session-start handler. Kept for tests and simple callers
    // that prefer a blocking API; production paths go through
    // HandleSessionStartAsync so the I/O thread is not held while the memory
    // store is touched. Returns OK if the session was provisioned (or can be)
    // and non-OK to reject the session-start frame.
    [[nodiscard]] absl::Status HandleSessionStart(const SessionStartRequestEvent& event) override;
    // Async session-start handler used by the transport. The responder hops
    // onto its worker pool, initializes per-session memory (with retry per
    // `session_start_persistence_max_attempts`), and then invokes
    // `on_complete` with OK to accept or a non-OK status to reject.
    void HandleSessionStartAsync(const SessionStartRequestEvent& event,
                                 GatewayEmitCallback on_complete) override;
    // Post-accept hook: called after the transport has emitted
    // `session.started`. Finalizes any state that had to wait for the session
    // to be live (e.g. pre-warming retrieval caches).
    void OnSessionStarted(const SessionStartedEvent& event) override;
    // Transcript seed handler. Persists the seeded message into the session's
    // working memory without invoking the LLM and returns OK on success. Used
    // by eval harnesses to backfill conversation history.
    [[nodiscard]] absl::Status HandleTranscriptSeed(const TranscriptSeedEvent& event) override;
    // Fires for every accepted user turn. The responder snapshots the turn
    // onto its internal queue and schedules execution on the session's
    // strand; the actual LLM call and reply emission happen asynchronously.
    void OnTurnAccepted(const TurnAcceptedEvent& event) override;
    // Fires when a client requests cancellation of an in-flight turn. Marks
    // the tracked turn as cancelled so the worker thread aborts at the next
    // safe point and emits a `turn.cancelled` frame.
    void OnTurnCancelRequested(const TurnCancelRequestedEvent& event) override;
    // Fires when a session closes (client disconnect, error, or orderly
    // shutdown). Tears down the per-session memory orchestrator and forgets
    // any tracked turns belonging to the session.
    void OnSessionClosed(const SessionClosedEvent& event) override;
    // Fires once from GatewayServer::Stop() before sessions are asked to
    // drain. The responder uses this hook to terminate accepted turns that
    // are still queued with a server-stopping error so they do not block
    // shutdown on the per-session grace period.
    void OnServerStopping(GatewaySessionRegistry& session_registry) override;
    // Appends a user message to the session's working memory out-of-band
    // (i.e. without going through the gateway turn pipeline). Intended for
    // test harnesses and eval replay that construct conversation history
    // directly. Returns NotFound if the session has no memory state.
    [[nodiscard]] absl::Status AppendSessionUserMessage(std::string_view session_id,
                                                        std::string_view turn_id,
                                                        std::string_view text);
    // Appends an assistant message to the session's working memory
    // out-of-band. Same semantics as AppendSessionUserMessage; used to seed
    // prior assistant replies into history for deterministic replay.
    [[nodiscard]] absl::Status AppendSessionAssistantMessage(std::string_view session_id,
                                                             std::string_view turn_id,
                                                             std::string_view text);
    // Renders the current system-prompt + working-memory context string for
    // the session, applying the configured byte budgets. Primarily a test
    // and debugging helper — production turns render implicitly as part of
    // OnTurnAccepted.
    [[nodiscard]] absl::StatusOr<std::string>
    RenderSessionMemoryPrompt(std::string_view session_id) const;
    // Returns a deep copy of the session's current working-memory state.
    // Callers that need a fully-settled snapshot (no pending mid-term
    // compactions in flight) should call AwaitSessionMemorySettled first.
    [[nodiscard]] absl::StatusOr<isla::server::memory::WorkingMemoryState>
    SnapshotSessionWorkingMemoryState(std::string_view session_id) const;
    // Blocks until all pending mid-term memory work for the session has completed and been applied.
    // Callers that need a settled memory snapshot should call this before
    // SnapshotSessionWorkingMemoryState.
    [[nodiscard]] absl::Status AwaitSessionMemorySettled(std::string_view session_id);
    // Runs the blocking sleep-cycle boundary for the session. Today this drains pending mid-term
    // work, clears the transient working set, and persists the reset snapshot.
    [[nodiscard]] absl::StatusOr<isla::server::memory::SleepCycleResult>
    RunSessionSleepCycle(std::string_view session_id, isla::server::memory::Timestamp cycle_time);
    // Synchronously drives an accepted turn through retrieval, LLM execution,
    // reply emission, and post-reply memory updates, returning the terminal
    // result. Used by tests and benchmarks that want to run turns without a
    // real client; production turns go through OnTurnAccepted + the worker.
    [[nodiscard]] absl::StatusOr<GatewayAcceptedTurnResult>
    RunAcceptedTurnToCompletion(const TurnAcceptedEvent& event);
    // Blocks until the responder's cumulative accepted-turn count reaches at
    // least `expected_count`, or until shutdown. Returns true on success,
    // false if the responder stopped before the count was reached. Used by
    // tests to synchronize on "all expected turns have been dispatched".
    [[nodiscard]] bool WaitForAcceptedTurns(std::size_t expected_count);
    // True when the config supplies an LLM pathway (either `llm_client` or
    // `openai_client`) that mid-term memory can drive. This is a necessary
    // but not sufficient condition: a missing memory store or a failure in
    // CreateMidTermMemoryComponents will still leave mid-term memory
    // unavailable at runtime. Use IsMidTermMemoryAvailable() to check
    // whether mid-term memory is actually usable.
    [[nodiscard]] bool IsMidTermMemoryConfigured() const;
    // True when mid-term memory is both configured AND its one-time
    // initialization (decider/compactor/extractor construction) succeeded.
    // False if configuration is missing or initialization returned a non-OK
    // status; see MidTermMemoryInitializationStatus for the failure detail.
    [[nodiscard]] bool IsMidTermMemoryAvailable() const;
    // Terminal status of mid-term memory initialization. OK if available or
    // not configured; a captured non-OK status explains why
    // CreateMidTermMemoryComponents() failed — for example an invalid
    // flush-decider or compactor model id, a missing required LLM pathway,
    // or an embedding client that failed its readiness check. Note that
    // MemoryStore reachability is NOT validated here; the store is only
    // touched at runtime on session start / persistence, so a broken store
    // will surface as a HandleSessionStart failure rather than through this
    // status. Stable for the lifetime of the responder — initialization is
    // attempted once at construction.
    [[nodiscard]] const absl::Status& MidTermMemoryInitializationStatus() const;

  private:
    using Clock = std::chrono::steady_clock;
    using WorkerPool = boost::asio::thread_pool;
    using WorkerExecutor = WorkerPool::executor_type;
    using SessionStrand = boost::asio::strand<WorkerExecutor>;

    struct SessionMemoryState {
        explicit SessionMemoryState(isla::server::memory::MemoryOrchestrator orchestrator_in)
            : orchestrator(std::move(orchestrator_in)) {}

        mutable std::mutex mutex;
        isla::server::memory::MemoryOrchestrator orchestrator;
    };

    struct PendingTurn {
        std::string session_id;
        std::string turn_id;
        std::string text;
        std::string rendered_system_prompt;
        std::string rendered_working_memory_context;
        std::shared_ptr<const TurnTelemetryContext> telemetry_context;
        Clock::time_point enqueued_at = Clock::time_point::min();
        Clock::time_point ready_at = Clock::time_point::min();
        bool cancel_requested = false;
    };

    struct PendingSessionStart {
        SessionStartRequestEvent event;
        GatewayEmitCallback on_complete;
        Clock::time_point ready_at = Clock::time_point::min();
        std::size_t attempts_used = 0;
    };

    struct LiveReplayClockState {
        std::optional<isla::server::memory::Timestamp> session_start_time;
        absl::flat_hash_map<std::string, isla::server::memory::Timestamp> conversation_times;
    };

    void StopWorker();
    void WorkerLoop();
    [[nodiscard]] std::shared_ptr<SessionStrand>
    FindOrCreateSessionStrandLocked(std::string_view session_id);
    void SchedulePendingSessionStart(PendingSessionStart session_start);
    void ScheduleAcceptedTurn(PendingTurn turn);
    void CompleteSessionStartCallback(GatewayEmitCallback on_complete, absl::Status status) const;
    void RecordDequeueTelemetry(const PendingTurn& turn, Clock::time_point dequeued_at) const;
    [[nodiscard]] bool TryMarkTrackedTurnCancelled(std::string_view session_id,
                                                   std::string_view turn_id);
    [[nodiscard]] bool IsTrackedTurnCancelled(std::string_view session_id,
                                              std::string_view turn_id) const;
    [[nodiscard]] bool ShouldAbortTrackedTurn(std::string_view session_id,
                                              std::string_view turn_id) const;
    void ForgetInProgressTurn(std::string_view session_id, std::string_view turn_id);
    [[nodiscard]] PendingTurn BuildPendingTurnShell(const TurnAcceptedEvent& event) const;
    [[nodiscard]] absl::StatusOr<PendingTurn> PrepareAcceptedTurn(const TurnAcceptedEvent& event);
    [[nodiscard]] std::optional<GatewayAcceptedTurnResult>
    ExecutePreparedTurnToTerminal(const PendingTurn& turn);
    void AsyncFinishServerStoppingTurn(const PendingTurn& turn);
    void BestEffortTerminateAcceptedTurn(const PendingTurn& turn, std::string_view code,
                                         std::string_view message,
                                         std::string_view log_context) noexcept;
    void FinishProcessingExceptionTurn(const PendingTurn& turn, std::string_view detail) noexcept;
    void FinishSuccessfulTurn(const PendingTurn& turn);
    void FinishCancelledTurn(const PendingTurn& turn);
    void FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                  const PendingTurn& turn);
    [[nodiscard]] GatewaySessionRegistry* session_registry() const;
    [[nodiscard]] std::shared_ptr<SessionMemoryState>
    FindSessionMemory(std::string_view session_id) const;
    [[nodiscard]] std::optional<absl::Status>
    FindSessionStartFailure(std::string_view session_id) const;
    void
    RecordSessionReplayClock(std::string_view session_id,
                             std::optional<isla::server::memory::Timestamp> session_start_time);
    void RecordConversationReplayTime(std::string_view session_id, std::string_view turn_id,
                                      isla::server::memory::MessageRole role,
                                      std::optional<isla::server::memory::Timestamp> create_time);
    [[nodiscard]] absl::Status InitializeSessionMemory(std::string_view session_id,
                                                       std::string_view user_id);
    [[nodiscard]] isla::server::memory::Timestamp
    ResolveSessionStartTime(std::string_view session_id) const;
    [[nodiscard]] isla::server::memory::Timestamp
    ResolveConversationMessageTime(std::string_view session_id, std::string_view turn_id,
                                   isla::server::memory::MessageRole role) const;
    [[nodiscard]] absl::StatusOr<isla::server::memory::UserQueryMemoryResult>
    HandleAcceptedTurnMemory(const TurnAcceptedEvent& event);
    [[nodiscard]] absl::Status HandleSuccessfulReplyMemory(const PendingTurn& turn,
                                                           std::string_view reply_text);
    [[nodiscard]] absl::StatusOr<std::string>
    ExpandMidTermEpisodeForSession(std::string_view session_id, std::string_view episode_id) const;
    [[nodiscard]] std::optional<isla::server::tools::ToolExecutionContext>
    BuildToolExecutionContext(std::string_view session_id,
                              std::shared_ptr<const TurnTelemetryContext> telemetry_context) const;

    GatewayStubResponderConfig config_;
    GatewayPlanExecutor executor_;
    WorkerPool worker_pool_;
    isla::server::memory::MidTermFlushDeciderPtr mid_term_flush_decider_;
    isla::server::memory::MidTermCompactorPtr mid_term_compactor_;
    isla::server::memory::SleepCycleSemanticExtractorPtr sleep_cycle_semantic_extractor_;
    isla::server::memory::RetrievedMemoryRerankerPtr retrieved_memory_reranker_;
    std::shared_ptr<const isla::server::EmbeddingClient> retrieval_embedding_client_;
    std::string retrieval_embedding_model_;
    bool mid_term_memory_configured_ = false;
    absl::Status mid_term_memory_initialization_status_ = absl::OkStatus();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    GatewaySessionRegistry* session_registry_ = nullptr;
    absl::flat_hash_map<std::string, std::shared_ptr<SessionStrand>> session_strands_;
    absl::flat_hash_map<std::string, PendingSessionStart> pending_session_starts_;
    absl::flat_hash_map<std::string, PendingTurn> pending_turns_;
    absl::flat_hash_map<std::string, PendingTurn> in_progress_turns_;
    absl::flat_hash_map<std::string, std::shared_ptr<SessionMemoryState>> memory_by_session_;
    absl::flat_hash_map<std::string, LiveReplayClockState> live_replay_clock_by_session_;
    absl::flat_hash_map<std::string, absl::Status> failed_session_starts_;
    bool stopping_ = false;
    bool worker_stop_requested_ = false;
    bool worker_pool_joined_ = false;
    std::size_t accepted_turns_count_ = 0;
};

} // namespace isla::server::ai_gateway
