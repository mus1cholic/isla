#include "isla/server/ai_gateway_stub_responder.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/ai_gateway_session_handler.hpp"
#include "isla/server/ai_gateway_stub_responder_utils.hpp"
#include "isla/server/gemini_api_embedding_client.hpp"
#include "isla/server/memory/mid_term_compactor.hpp"
#include "isla/server/memory/mid_term_flush_decider.hpp"
#include "isla/server/openai_llm_client.hpp"
#include "isla/server/tools/expand_mid_term_tool.hpp"
#include "isla/server/tools/tool_registry.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;
inline constexpr std::size_t kMaxRenderedSystemPromptBytes =
    static_cast<const std::size_t>(64U * 1024U);
inline constexpr std::size_t kMaxRenderedWorkingMemoryContextBytes =
    static_cast<const std::size_t>(64U * 1024U);
inline constexpr std::size_t kMaxRenderedPromptBytes =
    kMaxRenderedSystemPromptBytes + kMaxRenderedWorkingMemoryContextBytes;

GatewayAcceptedTurnResult FailedAcceptedTurnResult(std::string_view code,
                                                   std::string_view message) {
    return GatewayAcceptedTurnResult{
        .state = GatewayAcceptedTurnTerminalState::kFailed,
        .reply_text = std::nullopt,
        .failure =
            GatewayAcceptedTurnFailure{
                .code = std::string(code),
                .message = std::string(message),
            },
    };
}

GatewayAcceptedTurnResult CancelledAcceptedTurnResult() {
    return GatewayAcceptedTurnResult{
        .state = GatewayAcceptedTurnTerminalState::kCancelled,
        .reply_text = std::nullopt,
        .failure = std::nullopt,
    };
}

GatewayAcceptedTurnResult SucceededAcceptedTurnResult(std::string reply_text) {
    return GatewayAcceptedTurnResult{
        .state = GatewayAcceptedTurnTerminalState::kSucceeded,
        .reply_text = std::move(reply_text),
        .failure = std::nullopt,
    };
}

std::string ResolveMidTermFlushDeciderModel(const GatewayStubResponderConfig& config) {
    return config.llm_runtime_config.mid_term_flush_decider_model.empty()
               ? std::string(kDefaultMidTermFlushDeciderModel)
               : config.llm_runtime_config.mid_term_flush_decider_model;
}

std::string ResolveMidTermCompactorModel(const GatewayStubResponderConfig& config) {
    return config.llm_runtime_config.mid_term_compactor_model.empty()
               ? std::string(kDefaultMidTermCompactorModel)
               : config.llm_runtime_config.mid_term_compactor_model;
}

std::string ResolveMidTermEmbeddingModel(const GatewayStubResponderConfig& config) {
    return config.llm_runtime_config.mid_term_embedding_model.empty()
               ? std::string(kDefaultMidTermEmbeddingModel)
               : config.llm_runtime_config.mid_term_embedding_model;
}

class CallbackToolSessionReader final : public isla::server::tools::ToolSessionReader {
  public:
    using ExpandFn = std::function<absl::StatusOr<std::string>(std::string_view)>;

    explicit CallbackToolSessionReader(ExpandFn expand_fn) : expand_fn_(std::move(expand_fn)) {}

    [[nodiscard]] absl::StatusOr<std::string>
    ExpandMidTermEpisode(std::string_view episode_id) const override {
        return expand_fn_(episode_id);
    }

  private:
    ExpandFn expand_fn_;
};

std::shared_ptr<const isla::server::tools::ToolRegistry> CreateDefaultToolRegistry() {
    const absl::StatusOr<isla::server::tools::ToolRegistry> registry =
        isla::server::tools::ToolRegistry::Create(
            { std::make_shared<const isla::server::tools::ExpandMidTermTool>() });
    if (!registry.ok()) {
        LOG(ERROR) << "AI gateway failed to build default tool registry detail='"
                   << SanitizeForLog(registry.status().message()) << "'";
        return nullptr;
    }
    return std::make_shared<const isla::server::tools::ToolRegistry>(*registry);
}

GatewayStepRegistryConfig BuildExecutorConfig(const GatewayStubResponderConfig& config) {
    return GatewayStepRegistryConfig{
        .llm_runtime_config = config.llm_runtime_config,
        .openai_config = config.openai_config,
        .openai_client = config.openai_client,
        .llm_client = nullptr,
        .tool_registry = CreateDefaultToolRegistry(),
    };
}

template <typename StartFn>
absl::Status await_emit(std::chrono::milliseconds timeout, StartFn&& start) {
    auto promise = std::make_shared<std::promise<absl::Status>>();
    std::future<absl::Status> future = promise->get_future();
    start([promise](absl::Status status) { promise->set_value(std::move(status)); });
    if (future.wait_for(timeout) != std::future_status::ready) {
        return absl::DeadlineExceededError("timed out waiting for async emit completion");
    }
    return future.get();
}

bool IsRetryableSessionStartStatus(const absl::Status& status) {
    switch (status.code()) {
    case absl::StatusCode::kDeadlineExceeded:
    case absl::StatusCode::kUnavailable:
        return true;
    default:
        return false;
    }
}

std::string SessionStartErrorCode(const absl::Status& status) {
    switch (status.code()) {
    case absl::StatusCode::kDeadlineExceeded:
    case absl::StatusCode::kUnavailable:
        return "service_unavailable";
    default:
        return "internal_error";
    }
}

struct MidTermMemoryComponents {
    isla::server::memory::MidTermFlushDeciderPtr flush_decider;
    isla::server::memory::MidTermCompactorPtr compactor;
};

absl::StatusOr<MidTermMemoryComponents>
CreateMidTermMemoryComponents(const GatewayStubResponderConfig& config) {
    if (config.openai_client == nullptr) {
        return MidTermMemoryComponents{};
    }

    absl::StatusOr<std::shared_ptr<const isla::server::LlmClient>> llm_client =
        isla::server::CreateOpenAiLlmClient(config.openai_client);
    if (!llm_client.ok()) {
        return llm_client.status();
    }

    absl::StatusOr<isla::server::memory::MidTermFlushDeciderPtr> decider =
        isla::server::memory::CreateLlmMidTermFlushDecider(*llm_client,
                                                           ResolveMidTermFlushDeciderModel(config));
    if (!decider.ok()) {
        return decider.status();
    }

    std::shared_ptr<const isla::server::EmbeddingClient> embedding_client = config.embedding_client;
    if (embedding_client == nullptr && config.gemini_api_embedding_config.enabled) {
        absl::StatusOr<std::shared_ptr<const isla::server::EmbeddingClient>> created_embedding =
            isla::server::CreateGeminiApiEmbeddingClient(config.gemini_api_embedding_config);
        if (!created_embedding.ok()) {
            return created_embedding.status();
        }
        embedding_client = std::move(*created_embedding);
    }

    absl::StatusOr<isla::server::memory::MidTermCompactorPtr> compactor =
        isla::server::memory::CreateLlmMidTermCompactor(
            *llm_client, ResolveMidTermCompactorModel(config), std::move(embedding_client),
            config.gemini_api_embedding_config.enabled || config.embedding_client != nullptr
                ? ResolveMidTermEmbeddingModel(config)
                : std::string());
    if (!compactor.ok()) {
        return compactor.status();
    }

    return MidTermMemoryComponents{
        .flush_decider = std::move(*decider),
        .compactor = std::move(*compactor),
    };
}

} // namespace

GatewayStubResponder::GatewayStubResponder(GatewayStubResponderConfig config)
    : config_(std::move(config)), executor_(BuildExecutorConfig(config_)),
      mid_term_memory_configured_(config_.openai_client != nullptr) {

    const std::string flush_decider_model = ResolveMidTermFlushDeciderModel(config_);
    const std::string compactor_model = ResolveMidTermCompactorModel(config_);
    const std::string embedding_model = ResolveMidTermEmbeddingModel(config_);
    if (!mid_term_memory_configured_) {
        LOG(INFO) << "AI gateway stub mid-term memory not configured because no OpenAI responses"
                  << " client was provided";
    } else {
        absl::StatusOr<MidTermMemoryComponents> created_components =
            CreateMidTermMemoryComponents(config_);
        if (!created_components.ok()) {
            mid_term_memory_initialization_status_ = created_components.status();
            LOG(WARNING) << "AI gateway stub degraded mid-term memory to working-memory-only"
                         << " flush_decider_model=" << SanitizeForLog(flush_decider_model)
                         << " compactor_model=" << SanitizeForLog(compactor_model)
                         << " embedding_model=" << SanitizeForLog(embedding_model) << " detail='"
                         << SanitizeForLog(mid_term_memory_initialization_status_.message()) << "'";
        } else {
            mid_term_flush_decider_ = std::move(created_components->flush_decider);
            mid_term_compactor_ = std::move(created_components->compactor);
            LOG(INFO) << "AI gateway stub enabled mid-term memory flush_decider_model="
                      << SanitizeForLog(flush_decider_model)
                      << " compactor_model=" << SanitizeForLog(compactor_model)
                      << " embedding_model=" << SanitizeForLog(embedding_model);
        }
    }

    worker_ = std::thread([this] {
        try {
            WorkerLoop();
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway stub worker terminated with exception detail='"
                       << SanitizeForLog(error.what()) << "'";
        } catch (...) {
            LOG(ERROR) << "AI gateway stub worker terminated with unknown exception";
        }
    });
}

GatewayStubResponder::~GatewayStubResponder() {
    StopWorker();
}

void GatewayStubResponder::AttachSessionRegistry(GatewaySessionRegistry* session_registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_registry_ = session_registry;
}

bool GatewayStubResponder::IsMidTermMemoryConfigured() const {
    return mid_term_memory_configured_;
}

bool GatewayStubResponder::IsMidTermMemoryAvailable() const {
    return mid_term_memory_configured_ && mid_term_memory_initialization_status_.ok();
}

const absl::Status& GatewayStubResponder::MidTermMemoryInitializationStatus() const {
    return mid_term_memory_initialization_status_;
}

void GatewayStubResponder::OnSessionStarted(const SessionStartedEvent& event) {
    LOG(INFO) << "AI gateway stub observed session start session="
              << SanitizeForLog(event.session_id);
    const absl::Status status = InitializeSessionMemory(event.session_id);
    if (!status.ok()) {
        LOG(ERROR) << "AI gateway stub failed to initialize session memory session="
                   << SanitizeForLog(event.session_id) << " detail='"
                   << SanitizeForLog(status.message()) << "'";
        BestEffortEmitSessionStartFailure(event.session_id, status);
    }
}

GatewayStubResponder::PendingTurn
GatewayStubResponder::BuildPendingTurnShell(const TurnAcceptedEvent& event) const {
    return PendingTurn{
        .session_id = event.session_id,
        .turn_id = event.turn_id,
        .text = event.text,
        .rendered_system_prompt = "",
        .rendered_working_memory_context = "",
        .telemetry_context = event.telemetry_context,
        .ready_at = Clock::now(),
        .cancel_requested = false,
    };
}

absl::StatusOr<GatewayStubResponder::PendingTurn>
GatewayStubResponder::PrepareAcceptedTurn(const TurnAcceptedEvent& event) {
    ScopedTelemetryPhase memory_user_query_phase(
        event.telemetry_context, telemetry::kPhaseMemoryUserQuery,
        telemetry::kEventMemoryUserQueryStarted, telemetry::kEventMemoryUserQueryCompleted);
    const absl::StatusOr<isla::server::memory::UserQueryMemoryResult> memory_result =
        HandleAcceptedTurnMemory(event);
    if (!memory_result.ok()) {
        return memory_result.status();
    }
    VLOG(1) << "AI gateway stub prepared working memory session="
            << SanitizeForLog(event.session_id) << " turn_id=" << SanitizeForLog(event.turn_id)
            << " working_memory_bytes=" << memory_result->rendered_working_memory.size();
    memory_user_query_phase.Finish();

    PendingTurn turn = BuildPendingTurnShell(event);
    turn.rendered_system_prompt = memory_result->rendered_system_prompt;
    turn.rendered_working_memory_context = memory_result->rendered_working_memory_context;
    return turn;
}

void GatewayStubResponder::OnTurnAccepted(const TurnAcceptedEvent& event) {
    LOG(INFO) << "AI gateway stub accepted turn session=" << SanitizeForLog(event.session_id)
              << " turn_id=" << SanitizeForLog(event.turn_id);

    bool stopping = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++accepted_turns_count_;
        stopping = stopping_;
    }
    if (stopping) {
        AsyncFinishServerStoppingTurn(BuildPendingTurnShell(event));
        return;
    }

    const absl::StatusOr<PendingTurn> prepared_turn = PrepareAcceptedTurn(event);
    if (!prepared_turn.ok()) {
        LOG(ERROR) << "AI gateway stub failed to process turn memory session="
                   << SanitizeForLog(event.session_id)
                   << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='"
                   << SanitizeForLog(prepared_turn.status().message()) << "'";
        BestEffortTerminateAcceptedTurn(BuildPendingTurnShell(event), "internal_error",
                                        "stub responder failed to update memory",
                                        "memory update failure");
        return;
    }

    Clock::time_point enqueued_at = Clock::time_point::min();
    std::shared_ptr<const TurnTelemetryContext> enqueued_telemetry_context =
        event.telemetry_context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enqueued_at = Clock::now();
        VLOG(1) << "AI gateway stub queued turn session=" << event.session_id
                << " turn_id=" << SanitizeForLog(event.turn_id)
                << " delay_ms=" << config_.response_delay.count();
        PendingTurn turn = *prepared_turn;
        turn.enqueued_at = enqueued_at;
        turn.ready_at = Clock::now() + config_.response_delay;
        pending_turns_.insert_or_assign(event.session_id, std::move(turn));
    }
    RecordTelemetryEvent(enqueued_telemetry_context, telemetry::kEventTurnEnqueued, enqueued_at);

    cv_.notify_all();
}

void GatewayStubResponder::OnTurnCancelRequested(const TurnCancelRequestedEvent& event) {
    LOG(INFO) << "AI gateway stub received cancel session=" << SanitizeForLog(event.session_id)
              << " turn_id=" << SanitizeForLog(event.turn_id);

    if (!TryMarkTrackedTurnCancelled(event.session_id, event.turn_id)) {
        LOG(WARNING) << "AI gateway stub ignored cancel for untracked turn session="
                     << SanitizeForLog(event.session_id)
                     << " turn_id=" << SanitizeForLog(event.turn_id);
        return;
    }
}

void GatewayStubResponder::OnSessionClosed(const SessionClosedEvent& event) {
    LOG(INFO) << "AI gateway stub observed session close session="
              << SanitizeForLog(event.session_id) << " detail='" << SanitizeForLog(event.detail)
              << "'";

    std::lock_guard<std::mutex> lock(mutex_);
    const auto pending_erased = pending_turns_.erase(event.session_id);
    const auto in_progress_erased = in_progress_turns_.erase(event.session_id);
    memory_by_session_.erase(event.session_id);
    failed_session_starts_.erase(event.session_id);
    if (pending_erased > 0U || in_progress_erased > 0U) {
        VLOG(1) << "AI gateway stub dropped pending turn for closed session session="
                << SanitizeForLog(event.session_id);
    }
}

void GatewayStubResponder::OnServerStopping(GatewaySessionRegistry& session_registry) {
    AttachSessionRegistry(&session_registry);

    std::vector<PendingTurn> turns_to_finalize;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        turns_to_finalize.reserve(pending_turns_.size() + in_progress_turns_.size());
        for (const auto& entry : pending_turns_) {
            turns_to_finalize.push_back(entry.second);
        }
        for (const auto& entry : in_progress_turns_) {
            turns_to_finalize.push_back(entry.second);
        }
        pending_turns_.clear();
        in_progress_turns_.clear();
        memory_by_session_.clear();
        failed_session_starts_.clear();
    }

    VLOG(1) << "AI gateway stub stopping pending_turns=" << turns_to_finalize.size();

    StopWorker();

    for (const PendingTurn& turn : turns_to_finalize) {
        FinishServerStoppingTurn(session_registry, turn);
    }
}

absl::Status GatewayStubResponder::AppendSessionUserMessage(std::string_view session_id,
                                                            std::string_view turn_id,
                                                            std::string_view text) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory != nullptr) {
        std::lock_guard<std::mutex> lock(session_memory->mutex);
        const absl::StatusOr<isla::server::memory::UserQueryMemoryResult> result =
            session_memory->orchestrator.HandleUserQuery(isla::server::memory::GatewayUserQuery(
                std::string(session_id), std::string(turn_id), std::string(text),
                ResolveConversationMessageTime(session_id, turn_id,
                                               isla::server::memory::MessageRole::User)));
        if (!result.ok()) {
            return result.status();
        }
        return absl::OkStatus();
    }

    if (const std::optional<absl::Status> start_failure = FindSessionStartFailure(session_id);
        start_failure.has_value()) {
        return *start_failure;
    }
    return absl::FailedPreconditionError("missing memory orchestrator for started session");
}

absl::Status GatewayStubResponder::AppendSessionAssistantMessage(std::string_view session_id,
                                                                 std::string_view turn_id,
                                                                 std::string_view text) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory != nullptr) {
        std::lock_guard<std::mutex> lock(session_memory->mutex);
        return session_memory->orchestrator.HandleAssistantReply(
            isla::server::memory::GatewayAssistantReply(
                std::string(session_id), std::string(turn_id), std::string(text),
                ResolveConversationMessageTime(session_id, turn_id,
                                               isla::server::memory::MessageRole::Assistant)));
    }

    if (const std::optional<absl::Status> start_failure = FindSessionStartFailure(session_id);
        start_failure.has_value()) {
        return *start_failure;
    }
    return absl::FailedPreconditionError("missing memory orchestrator for started session");
}

absl::StatusOr<GatewayAcceptedTurnResult>
GatewayStubResponder::RunAcceptedTurnToCompletion(const TurnAcceptedEvent& event) {
    LOG(INFO) << "AI gateway stub directly executing accepted turn session="
              << SanitizeForLog(event.session_id) << " turn_id=" << SanitizeForLog(event.turn_id);

    bool stopping = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++accepted_turns_count_;
        stopping = stopping_;
    }
    cv_.notify_all();
    if (stopping) {
        PendingTurn stopping_turn = BuildPendingTurnShell(event);
        if (GatewaySessionRegistry* registry = session_registry(); registry != nullptr) {
            FinishServerStoppingTurn(*registry, stopping_turn);
        }
        return FailedAcceptedTurnResult("server_stopping", "server stopping");
    }

    const absl::StatusOr<PendingTurn> prepared_turn = PrepareAcceptedTurn(event);
    if (!prepared_turn.ok()) {
        LOG(ERROR) << "AI gateway stub failed to process direct turn memory session="
                   << SanitizeForLog(event.session_id)
                   << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='"
                   << SanitizeForLog(prepared_turn.status().message()) << "'";
        BestEffortTerminateAcceptedTurn(BuildPendingTurnShell(event), "internal_error",
                                        "stub responder failed to update memory",
                                        "memory update failure");
        return FailedAcceptedTurnResult("internal_error", "stub responder failed to update memory");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_progress_turns_.insert_or_assign(event.session_id, *prepared_turn);
    }

    std::optional<GatewayAcceptedTurnResult> result;
    try {
        result = ExecutePreparedTurnToTerminal(*prepared_turn);
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway stub direct turn processing threw session="
                   << SanitizeForLog(event.session_id)
                   << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='"
                   << SanitizeForLog(error.what()) << "'";
        FinishProcessingExceptionTurn(*prepared_turn, error.what());
        result = FailedAcceptedTurnResult("internal_error", "stub responder processing failed");
    } catch (...) {
        LOG(ERROR) << "AI gateway stub direct turn processing threw session="
                   << SanitizeForLog(event.session_id)
                   << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='unknown exception'";
        FinishProcessingExceptionTurn(*prepared_turn, "unknown exception");
        result = FailedAcceptedTurnResult("internal_error", "stub responder processing failed");
    }
    ForgetInProgressTurn(event.session_id, event.turn_id);

    if (!result.has_value()) {
        return FailedAcceptedTurnResult("server_stopping", "server stopping");
    }
    return *result;
}

void GatewayStubResponder::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void GatewayStubResponder::RecordDequeueTelemetry(const PendingTurn& turn,
                                                  Clock::time_point dequeued_at) const {
    if (turn.enqueued_at != Clock::time_point::min()) {
        RecordTelemetryPhase(turn.telemetry_context, telemetry::kPhaseQueueWait, turn.enqueued_at,
                             dequeued_at);
    }
    RecordTelemetryEvent(turn.telemetry_context, telemetry::kEventTurnDequeued, dequeued_at);
}

void GatewayStubResponder::WorkerLoop() {
    for (;;) {
        std::optional<PendingTurn> next_turn;
        std::optional<Clock::time_point> next_deadline;
        std::optional<PendingTurn> dequeued_turn;
        Clock::time_point dequeued_at = Clock::time_point::min();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (worker_stop_requested_) {
                    return;
                }

                const Clock::time_point now = Clock::now();
                for (auto it = pending_turns_.begin(); it != pending_turns_.end(); ++it) {
                    PendingTurn& turn = it->second;
                    if (turn.cancel_requested) {
                        next_turn = turn;
                        dequeued_turn = turn;
                        dequeued_at = now;
                        VLOG(1) << "AI gateway stub dequeued cancellation session="
                                << SanitizeForLog(it->first)
                                << " turn_id=" << SanitizeForLog(turn.turn_id);
                        in_progress_turns_.insert_or_assign(it->first, turn);
                        pending_turns_.erase(it);
                        break;
                    }
                    if (turn.ready_at <= now) {
                        next_turn = turn;
                        dequeued_turn = turn;
                        dequeued_at = now;
                        VLOG(1) << "AI gateway stub dequeued ready turn session="
                                << SanitizeForLog(it->first)
                                << " turn_id=" << SanitizeForLog(turn.turn_id);
                        in_progress_turns_.insert_or_assign(it->first, turn);
                        pending_turns_.erase(it);
                        break;
                    }
                    if (!next_deadline.has_value() || turn.ready_at < *next_deadline) {
                        next_deadline = turn.ready_at;
                    }
                }

                if (next_turn.has_value()) {
                    break;
                }

                if (next_deadline.has_value()) {
                    cv_.wait_until(lock, *next_deadline);
                } else {
                    cv_.wait(lock);
                }
                next_deadline.reset();
            }
        }

        if (dequeued_turn.has_value()) {
            RecordDequeueTelemetry(*dequeued_turn, dequeued_at);
        }

        if (!next_turn.has_value()) {
            continue;
        }

        // Phase 2.5 keeps a single blocking worker so stub turn completion stays deterministic and
        // transport orchestration remains simple before later executor/provider phases add richer
        // concurrency.
        // TODO(ai-gateway): Remove this global head-of-line blocking path. Slow step execution or
        // bounded-but-slow accepted-turn emit waits in one session currently hold the only worker
        // thread and can delay unrelated sessions. Move execution/emission ownership to a design
        // that isolates sessions or otherwise bounds cross-session impact.
        try {
            if (next_turn->cancel_requested) {
                FinishCancelledTurn(*next_turn);
            } else {
                FinishSuccessfulTurn(*next_turn);
            }
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway stub turn processing threw session=" << next_turn->session_id
                       << " turn_id=" << SanitizeForLog(next_turn->turn_id) << " detail='"
                       << SanitizeForLog(error.what()) << "'";
            FinishProcessingExceptionTurn(*next_turn, error.what());
        } catch (...) {
            LOG(ERROR) << "AI gateway stub turn processing threw session=" << next_turn->session_id
                       << " turn_id=" << SanitizeForLog(next_turn->turn_id)
                       << " detail='unknown exception'";
            FinishProcessingExceptionTurn(*next_turn, "unknown exception");
        }
        ForgetInProgressTurn(next_turn->session_id, next_turn->turn_id);
    }
}

bool GatewayStubResponder::TryMarkTrackedTurnCancelled(std::string_view session_id,
                                                       std::string_view turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto pending = pending_turns_.find(std::string(session_id));
        pending != pending_turns_.end()) {
        if (pending->second.turn_id != turn_id) {
            return false;
        }
        pending->second.ready_at = Clock::now();
        pending->second.cancel_requested = true;
        VLOG(1) << "AI gateway stub queued cancellation session=" << SanitizeForLog(session_id)
                << " turn_id=" << SanitizeForLog(turn_id);
        cv_.notify_all();
        return true;
    }

    if (auto in_progress = in_progress_turns_.find(std::string(session_id));
        in_progress != in_progress_turns_.end()) {
        if (in_progress->second.turn_id != turn_id) {
            return false;
        }
        in_progress->second.cancel_requested = true;
        VLOG(1) << "AI gateway stub marked in-progress cancellation session="
                << SanitizeForLog(session_id) << " turn_id=" << SanitizeForLog(turn_id);
        return true;
    }

    return false;
}

bool GatewayStubResponder::IsTrackedTurnCancelled(std::string_view session_id,
                                                  std::string_view turn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto in_progress = in_progress_turns_.find(std::string(session_id));
    return in_progress != in_progress_turns_.end() && in_progress->second.turn_id == turn_id &&
           in_progress->second.cancel_requested;
}

bool GatewayStubResponder::ShouldAbortTrackedTurn(std::string_view session_id,
                                                  std::string_view turn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
        return false;
    }
    const auto in_progress = in_progress_turns_.find(std::string(session_id));
    return in_progress == in_progress_turns_.end() || in_progress->second.turn_id != turn_id;
}

void GatewayStubResponder::ForgetInProgressTurn(std::string_view session_id,
                                                std::string_view turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = in_progress_turns_.find(std::string(session_id));
    if (it != in_progress_turns_.end() && it->second.turn_id == turn_id) {
        in_progress_turns_.erase(it);
    }
}

void GatewayStubResponder::AsyncFinishServerStoppingTurn(const PendingTurn& turn) {
    const std::shared_ptr<GatewayLiveSession> live_session =
        session_registry() == nullptr ? nullptr : session_registry()->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session during async shutdown session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.cancel_requested) {
        VLOG(1) << "AI gateway stub asynchronously emitting shutdown cancellation session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        live_session->AsyncEmitTurnCancelled(
            turn.turn_id, [session_id = std::string(turn.session_id),
                           turn_id = std::string(turn.turn_id)](absl::Status status) {
                if (!status.ok()) {
                    LOG(WARNING) << "AI gateway stub failed async shutdown cancellation session="
                                 << SanitizeForLog(session_id)
                                 << " turn_id=" << SanitizeForLog(turn_id) << " detail='"
                                 << SanitizeForLog(status.message()) << "'";
                }
            });
        return;
    }

    VLOG(1) << "AI gateway stub asynchronously emitting shutdown terminal error session="
            << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
    live_session->AsyncEmitError(
        turn.turn_id, "server_stopping", "server stopping",
        [live_session, session_id = std::string(turn.session_id),
         turn_id = std::string(turn.turn_id)](absl::Status status) {
            if (!status.ok()) {
                LOG(WARNING) << "AI gateway stub failed async shutdown error session="
                             << SanitizeForLog(session_id) << " turn_id=" << SanitizeForLog(turn_id)
                             << " detail='" << SanitizeForLog(status.message()) << "'";
                return;
            }
            live_session->AsyncEmitTurnCompleted(
                turn_id, [session_id = std::string(session_id),
                          turn_id = std::string(turn_id)](absl::Status completion_status) {
                    if (!completion_status.ok()) {
                        LOG(WARNING)
                            << "AI gateway stub failed async shutdown completion session="
                            << SanitizeForLog(session_id) << " turn_id=" << SanitizeForLog(turn_id)
                            << " detail='" << SanitizeForLog(completion_status.message()) << "'";
                    }
                });
        });
}

void GatewayStubResponder::BestEffortTerminateAcceptedTurn(const PendingTurn& turn,
                                                           std::string_view code,
                                                           std::string_view message,
                                                           std::string_view log_context) noexcept {
    try {
        RecordTelemetryEvent(turn.telemetry_context, telemetry::kEventTurnFailed);
        GatewaySessionRegistry* registry = session_registry();
        if (registry == nullptr) {
            LOG(WARNING) << "AI gateway stub missing session registry during " << log_context
                         << " session=" << SanitizeForLog(turn.session_id)
                         << " turn_id=" << SanitizeForLog(turn.turn_id);
            return;
        }

        const std::shared_ptr<GatewayLiveSession> live_session =
            registry->FindSession(turn.session_id);
        if (live_session == nullptr) {
            LOG(WARNING) << "AI gateway stub lost live session during " << log_context
                         << " session=" << SanitizeForLog(turn.session_id)
                         << " turn_id=" << SanitizeForLog(turn.turn_id);
            return;
        }

        if (!code.empty() && !message.empty()) {
            ScopedTelemetryPhase emit_error_phase(
                turn.telemetry_context, telemetry::kPhaseEmitError,
                telemetry::kEventErrorEmitStarted, telemetry::kEventErrorEmitCompleted);
            const absl::Status error_status =
                await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                    live_session->AsyncEmitError(turn.turn_id, std::string(code),
                                                 std::string(message), std::move(on_complete));
                });
            emit_error_phase.Finish();
            if (!error_status.ok()) {
                LOG(WARNING) << "AI gateway stub failed to emit follow-up error during "
                             << log_context << " session=" << SanitizeForLog(turn.session_id)
                             << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                             << SanitizeForLog(error_status.message()) << "'";
            }
        }

        ScopedTelemetryPhase completion_phase(
            turn.telemetry_context, telemetry::kPhaseEmitTurnCompleted,
            telemetry::kEventTurnCompletedEmitStarted, telemetry::kEventTurnCompletedEmitCompleted);
        const absl::Status completion_status =
            await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
            });
        const Clock::time_point finished_at = Clock::now();
        completion_phase.Finish(finished_at);
        RecordTurnFinished(turn.telemetry_context, telemetry::kOutcomeFailed, finished_at);
        if (!completion_status.ok()) {
            LOG(WARNING) << "AI gateway stub failed to emit follow-up completion during "
                         << log_context << " session=" << SanitizeForLog(turn.session_id)
                         << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                         << SanitizeForLog(completion_status.message()) << "'";
        }
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway stub follow-up terminalization failed during " << log_context
                   << " session=" << SanitizeForLog(turn.session_id)
                   << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                   << SanitizeForLog(error.what()) << "'";
    } catch (...) {
        LOG(ERROR) << "AI gateway stub follow-up terminalization failed during " << log_context
                   << " session=" << SanitizeForLog(turn.session_id)
                   << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='unknown exception'";
    }
}

void GatewayStubResponder::BestEffortEmitSessionStartFailure(std::string_view session_id,
                                                             const absl::Status& status) noexcept {
    try {
        GatewaySessionRegistry* registry = session_registry();
        if (registry == nullptr) {
            LOG(WARNING) << "AI gateway stub missing session registry during session start failure"
                         << " session=" << SanitizeForLog(session_id);
            return;
        }

        const std::shared_ptr<GatewayLiveSession> live_session = registry->FindSession(session_id);
        if (live_session == nullptr) {
            LOG(WARNING) << "AI gateway stub lost live session during session start failure"
                         << " session=" << SanitizeForLog(session_id);
            return;
        }

        const absl::Status emit_status =
            await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                live_session->AsyncEmitError(std::nullopt, SessionStartErrorCode(status),
                                             "failed to initialize session memory",
                                             std::move(on_complete));
            });
        if (!emit_status.ok()) {
            LOG(WARNING) << "AI gateway stub failed to emit session start error session="
                         << SanitizeForLog(session_id) << " detail='"
                         << SanitizeForLog(emit_status.message()) << "'";
        }
    } catch (const std::exception& error) {
        LOG(WARNING) << "AI gateway stub threw while emitting session start error session="
                     << SanitizeForLog(session_id) << " detail='" << SanitizeForLog(error.what())
                     << "'";
    } catch (...) {
        LOG(WARNING) << "AI gateway stub threw unknown exception while emitting session start "
                        "error session="
                     << SanitizeForLog(session_id);
    }
}

void GatewayStubResponder::FinishProcessingExceptionTurn(const PendingTurn& turn,
                                                         std::string_view detail) noexcept {
    BestEffortTerminateAcceptedTurn(turn, "internal_error", "stub responder processing failed",
                                    "processing failure");
    VLOG(1) << "AI gateway stub terminalized failed turn session="
            << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
            << " detail='" << SanitizeForLog(detail) << "'";
}

std::optional<GatewayAcceptedTurnResult>
GatewayStubResponder::ExecutePreparedTurnToTerminal(const PendingTurn& turn) {
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted successful turn during shutdown session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        return std::nullopt;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        return CancelledAcceptedTurnResult();
    }

    GatewaySessionRegistry* registry = session_registry();
    if (registry == nullptr) {
        LOG(WARNING) << "AI gateway stub missing session registry for session="
                     << SanitizeForLog(turn.session_id);
        return std::nullopt;
    }

    if (turn.text.size() > kMaxTextInputBytes) {
        LOG(ERROR) << "AI gateway stub rejected oversized accepted turn session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " text_bytes=" << turn.text.size();
        BestEffortTerminateAcceptedTurn(turn, "bad_request",
                                        "text.input text exceeds maximum length", "oversized turn");
        return FailedAcceptedTurnResult("bad_request", "text.input text exceeds maximum length");
    }
    if (turn.rendered_system_prompt.size() > kMaxRenderedSystemPromptBytes) {
        LOG(ERROR) << "AI gateway stub rejected oversized rendered system prompt session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " system_prompt_bytes=" << turn.rendered_system_prompt.size()
                   << " budget_bytes=" << kMaxRenderedSystemPromptBytes;
        BestEffortTerminateAcceptedTurn(turn, "bad_request",
                                        "rendered system prompt exceeds maximum length",
                                        "oversized rendered system prompt");
        return FailedAcceptedTurnResult("bad_request",
                                        "rendered system prompt exceeds maximum length");
    }
    if (turn.rendered_working_memory_context.size() > kMaxRenderedWorkingMemoryContextBytes) {
        LOG(ERROR) << "AI gateway stub rejected oversized rendered working memory context session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " context_bytes=" << turn.rendered_working_memory_context.size()
                   << " budget_bytes=" << kMaxRenderedWorkingMemoryContextBytes;
        BestEffortTerminateAcceptedTurn(turn, "bad_request",
                                        "rendered working memory context exceeds maximum length",
                                        "oversized rendered context");
        return FailedAcceptedTurnResult("bad_request",
                                        "rendered working memory context exceeds maximum length");
    }
    if (turn.rendered_system_prompt.size() + turn.rendered_working_memory_context.size() >
        kMaxRenderedPromptBytes) {
        LOG(ERROR) << "AI gateway stub rejected oversized rendered prompt session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " system_prompt_bytes=" << turn.rendered_system_prompt.size()
                   << " context_bytes=" << turn.rendered_working_memory_context.size()
                   << " budget_bytes=" << kMaxRenderedPromptBytes;
        BestEffortTerminateAcceptedTurn(turn, "bad_request",
                                        "rendered prompt exceeds maximum combined length",
                                        "oversized rendered prompt");
        return FailedAcceptedTurnResult("bad_request",
                                        "rendered prompt exceeds maximum combined length");
    }

    ScopedTelemetryPhase plan_create_phase(turn.telemetry_context, telemetry::kPhasePlanCreate,
                                           telemetry::kEventPlanCreateStarted,
                                           telemetry::kEventPlanCreateCompleted);
    const absl::StatusOr<ExecutionPlan> execution_plan = CreateOpenAiPlan();
    plan_create_phase.Finish();
    if (!execution_plan.ok()) {
        LOG(ERROR) << "AI gateway stub planner rejected accepted turn session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " detail='" << SanitizeForLog(execution_plan.status().message()) << "'";
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder failed to plan request", "planner failure");
        return FailedAcceptedTurnResult("internal_error", "stub responder failed to plan request");
    }
    if (config_.on_execution_plan) {
        try {
            config_.on_execution_plan(*execution_plan);
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway stub planner hook threw session="
                       << SanitizeForLog(turn.session_id)
                       << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                       << SanitizeForLog(error.what()) << "'";
        } catch (...) {
            LOG(ERROR) << "AI gateway stub planner hook threw session="
                       << SanitizeForLog(turn.session_id)
                       << " turn_id=" << SanitizeForLog(turn.turn_id)
                       << " detail='unknown exception'";
        }
    }

    const ExecutionOutcome executor_outcome =
        executor_.Execute(*execution_plan, ExecutionRuntimeInput{
                                               .system_prompt = turn.rendered_system_prompt,
                                               .user_text = turn.rendered_working_memory_context,
                                               .telemetry_context = turn.telemetry_context,
                                               .tool_execution_context = BuildToolExecutionContext(
                                                   turn.session_id, turn.telemetry_context),
                                           });
    if (const auto* failure = std::get_if<ExecutionFailure>(&executor_outcome)) {
        LOG(ERROR) << "AI gateway stub executor failed session=" << SanitizeForLog(turn.session_id)
                   << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " step_name=" << SanitizeForLog(failure->step_name)
                   << " code=" << SanitizeForLog(failure->code)
                   << " retryable=" << (failure->retryable ? "true" : "false") << " detail='"
                   << SanitizeForLog(failure->message) << "'";
        BestEffortTerminateAcceptedTurn(turn, failure->code, failure->message, "executor failure");
        return FailedAcceptedTurnResult(failure->code, failure->message);
    }
    const auto& result = std::get<ExecutionResult>(executor_outcome);
    const auto* last_result = result.step_results.empty()
                                  ? nullptr
                                  : std::get_if<LlmCallResult>(&result.step_results.back());
    if (last_result == nullptr) {
        LOG(ERROR) << "AI gateway stub executor returned unexpected final result session="
                   << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id)
                   << " step_result_count=" << result.step_results.size();
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder received unexpected execution result",
                                        "executor result contract failure");
        return FailedAcceptedTurnResult("internal_error",
                                        "stub responder received unexpected execution result");
    }
    const std::string& reply = last_result->output_text;
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted reply emission during shutdown session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        return std::nullopt;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        return CancelledAcceptedTurnResult();
    }
    const std::shared_ptr<GatewayLiveSession> reply_session =
        registry->FindSession(turn.session_id);
    if (reply_session == nullptr || reply_session->is_closed()) {
        LOG(WARNING) << "AI gateway stub lost live session before text output session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return std::nullopt;
    }
    VLOG(1) << "AI gateway stub emitting successful reply session="
            << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
    ScopedTelemetryPhase emit_text_output_phase(
        turn.telemetry_context, telemetry::kPhaseEmitTextOutput,
        telemetry::kEventTextOutputEmitStarted, telemetry::kEventTextOutputEmitCompleted);
    absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            reply_session->AsyncEmitTextOutput(turn.turn_id, reply, std::move(on_complete));
        });
    emit_text_output_phase.Finish();
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit text output session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder failed to emit text output",
                                        "text output failure");
        return FailedAcceptedTurnResult("internal_error",
                                        "stub responder failed to emit text output");
    }

    ScopedTelemetryPhase memory_assistant_reply_phase(
        turn.telemetry_context, telemetry::kPhaseMemoryAssistantReply,
        telemetry::kEventMemoryAssistantReplyStarted,
        telemetry::kEventMemoryAssistantReplyCompleted);
    status = HandleSuccessfulReplyMemory(turn, reply);
    memory_assistant_reply_phase.Finish();
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to record assistant reply in memory session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        BestEffortTerminateAcceptedTurn(turn, "internal_error",
                                        "stub responder failed to update memory",
                                        "memory update failure");
        GatewayAcceptedTurnResult result =
            FailedAcceptedTurnResult("internal_error", "stub responder failed to update memory");
        result.reply_text = reply;
        return result;
    }

    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted completion during shutdown session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        return std::nullopt;
    }
    if (IsTrackedTurnCancelled(turn.session_id, turn.turn_id)) {
        FinishCancelledTurn(turn);
        GatewayAcceptedTurnResult result = CancelledAcceptedTurnResult();
        result.reply_text = reply;
        return result;
    }
    const std::shared_ptr<GatewayLiveSession> completion_session =
        registry->FindSession(turn.session_id);
    if (completion_session == nullptr || completion_session->is_closed()) {
        LOG(WARNING) << "AI gateway stub lost live session before completion session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return std::nullopt;
    }

    ScopedTelemetryPhase completion_phase(
        turn.telemetry_context, telemetry::kPhaseEmitTurnCompleted,
        telemetry::kEventTurnCompletedEmitStarted, telemetry::kEventTurnCompletedEmitCompleted);
    status = await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
        completion_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    const Clock::time_point finished_at = Clock::now();
    completion_phase.Finish(finished_at);
    RecordTurnFinished(turn.telemetry_context, telemetry::kOutcomeSucceeded, finished_at);
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn completion session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
    }
    return SucceededAcceptedTurnResult(reply);
}

void GatewayStubResponder::FinishSuccessfulTurn(const PendingTurn& turn) {
    static_cast<void>(ExecutePreparedTurnToTerminal(turn));
}

void GatewayStubResponder::FinishCancelledTurn(const PendingTurn& turn) {
    if (ShouldAbortTrackedTurn(turn.session_id, turn.turn_id)) {
        VLOG(1) << "AI gateway stub aborted cancellation during shutdown session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    GatewaySessionRegistry* registry = session_registry();
    if (registry == nullptr) {
        LOG(WARNING) << "AI gateway stub missing session registry for cancel session="
                     << SanitizeForLog(turn.session_id);
        return;
    }

    const std::shared_ptr<GatewayLiveSession> live_session = registry->FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session before cancel session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    VLOG(1) << "AI gateway stub emitting cancellation session=" << SanitizeForLog(turn.session_id)
            << " turn_id=" << SanitizeForLog(turn.turn_id);
    ScopedTelemetryPhase cancellation_phase(
        turn.telemetry_context, telemetry::kPhaseEmitTurnCancelled,
        telemetry::kEventTurnCancelledEmitStarted, telemetry::kEventTurnCancelledEmitCompleted);
    const absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            live_session->AsyncEmitTurnCancelled(turn.turn_id, std::move(on_complete));
        });
    const Clock::time_point finished_at = Clock::now();
    cancellation_phase.Finish(finished_at);
    RecordTurnFinished(turn.telemetry_context, telemetry::kOutcomeCancelled, finished_at);
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit turn cancelled session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
    }
}

void GatewayStubResponder::FinishServerStoppingTurn(GatewaySessionRegistry& session_registry,
                                                    const PendingTurn& turn) {
    const std::shared_ptr<GatewayLiveSession> live_session =
        session_registry.FindSession(turn.session_id);
    if (live_session == nullptr) {
        LOG(WARNING) << "AI gateway stub lost live session during shutdown session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id);
        return;
    }

    if (turn.cancel_requested) {
        VLOG(1) << "AI gateway stub emitting shutdown cancellation session="
                << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
        const absl::Status status =
            await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
                live_session->AsyncEmitTurnCancelled(turn.turn_id, std::move(on_complete));
            });
        if (!status.ok()) {
            LOG(WARNING) << "AI gateway stub failed to emit shutdown cancellation session="
                         << SanitizeForLog(turn.session_id)
                         << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                         << SanitizeForLog(status.message()) << "'";
        }
        return;
    }

    VLOG(1) << "AI gateway stub emitting shutdown terminal error session="
            << SanitizeForLog(turn.session_id) << " turn_id=" << SanitizeForLog(turn.turn_id);
    absl::Status status =
        await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
            live_session->AsyncEmitError(turn.turn_id, "server_stopping", "server stopping",
                                         std::move(on_complete));
        });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown error session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
        return;
    }

    status = await_emit(config_.async_emit_timeout, [&](GatewayEmitCallback on_complete) {
        live_session->AsyncEmitTurnCompleted(turn.turn_id, std::move(on_complete));
    });
    if (!status.ok()) {
        LOG(WARNING) << "AI gateway stub failed to emit shutdown completion session="
                     << SanitizeForLog(turn.session_id)
                     << " turn_id=" << SanitizeForLog(turn.turn_id) << " detail='"
                     << SanitizeForLog(status.message()) << "'";
    }
}

GatewaySessionRegistry* GatewayStubResponder::session_registry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_registry_;
}

std::shared_ptr<GatewayStubResponder::SessionMemoryState>
GatewayStubResponder::FindSessionMemory(std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = memory_by_session_.find(std::string(session_id));
    if (it == memory_by_session_.end()) {
        return nullptr;
    }
    return it->second;
}

std::optional<absl::Status>
GatewayStubResponder::FindSessionStartFailure(std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = failed_session_starts_.find(std::string(session_id));
    if (it == failed_session_starts_.end()) {
        return std::nullopt;
    }
    return it->second;
}

absl::Status GatewayStubResponder::InitializeSessionMemory(std::string_view session_id) {
    std::shared_ptr<SessionMemoryState> session_memory;
    const Clock::time_point initialization_started_at = Clock::now();
    {
        // Hold the lifecycle mutex across construction/insertion so OnSessionClosed cannot erase
        // the session and then have us recreate memory for an already-closed session. The store
        // upsert below runs after insertion so connect-time persistence does not block unrelated
        // lifecycle work behind this mutex.
        std::lock_guard<std::mutex> lock(mutex_);
        absl::StatusOr<isla::server::memory::MemoryOrchestrator> orchestrator =
            isla::server::memory::MemoryOrchestrator::Create(
                std::string(session_id), isla::server::memory::MemoryOrchestratorInit{
                                             .user_id = config_.memory_user_id,
                                             .store = config_.memory_store,
                                             .mid_term_flush_decider = mid_term_flush_decider_,
                                             .mid_term_compactor = mid_term_compactor_,
                                         });
        if (!orchestrator.ok()) {
            failed_session_starts_.insert_or_assign(std::string(session_id), orchestrator.status());
            return orchestrator.status();
        }
        session_memory = std::make_shared<SessionMemoryState>(std::move(*orchestrator));
        const auto [it, inserted] =
            memory_by_session_.try_emplace(std::string(session_id), session_memory);
        static_cast<void>(it);
        if (!inserted) {
            const absl::Status status =
                absl::AlreadyExistsError("memory orchestrator already exists for session");
            failed_session_starts_.insert_or_assign(std::string(session_id), status);
            return status;
        }
        failed_session_starts_.erase(std::string(session_id));
    }

    const isla::server::memory::Timestamp session_start_time = ResolveSessionStartTime(session_id);
    const std::size_t max_attempts = config_.session_start_persistence_max_attempts == 0U
                                         ? 1U
                                         : config_.session_start_persistence_max_attempts;
    absl::Status final_status = absl::OkStatus();
    std::size_t attempts_used = 0;
    for (std::size_t attempt = 1; attempt <= max_attempts; ++attempt) {
        attempts_used = attempt;
        {
            std::lock_guard<std::mutex> session_lock(session_memory->mutex);
            final_status = session_memory->orchestrator.BeginSession(session_start_time);
        }
        if (final_status.ok()) {
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         Clock::now() - initialization_started_at)
                                         .count();
            if (attempts_used == 1U) {
                VLOG(1) << "AI gateway stub initialized session memory session="
                        << SanitizeForLog(session_id);
            } else {
                LOG(INFO) << "AI gateway stub initialized session memory after retry session="
                          << SanitizeForLog(session_id) << " attempts=" << attempts_used
                          << " max_attempts=" << max_attempts << " duration_ms=" << duration_ms;
            }
            return absl::OkStatus();
        }
        if (!IsRetryableSessionStartStatus(final_status) || attempt == max_attempts) {
            break;
        }
        LOG(WARNING) << "AI gateway stub retrying session memory initialization session="
                     << SanitizeForLog(session_id) << " attempt=" << attempt
                     << " max_attempts=" << max_attempts << " detail='"
                     << SanitizeForLog(final_status.message()) << "'";
        if (config_.session_start_persistence_retry_delay.count() > 0) {
            // NOTICE: This blocks the shared gateway event thread between retries. We keep the
            // startup path synchronous for now so session persistence either succeeds or fails
            // before later turns rely on it. Replace this with an async timer-based retry once
            // session.start supports app-level accept/reject without emitting `session.started`
            // first.
            std::this_thread::sleep_for(config_.session_start_persistence_retry_delay);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        memory_by_session_.erase(std::string(session_id));
        failed_session_starts_.insert_or_assign(std::string(session_id), final_status);
    }
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - initialization_started_at)
                                 .count();
    if (IsRetryableSessionStartStatus(final_status)) {
        LOG(ERROR) << "AI gateway stub exhausted session memory initialization retries session="
                   << SanitizeForLog(session_id) << " attempts=" << max_attempts
                   << " duration_ms=" << duration_ms << " detail='"
                   << SanitizeForLog(final_status.message()) << "'";
    } else {
        LOG(ERROR) << "AI gateway stub hit non-retryable session memory initialization failure "
                      "session="
                   << SanitizeForLog(session_id) << " attempts=" << attempts_used
                   << " duration_ms=" << duration_ms << " detail='"
                   << SanitizeForLog(final_status.message()) << "'";
    }
    return final_status;
}

isla::server::memory::Timestamp
GatewayStubResponder::ResolveSessionStartTime(std::string_view session_id) const {
    if (config_.session_clock != nullptr) {
        if (const std::optional<isla::server::memory::Timestamp> overridden =
                config_.session_clock->ResolveSessionStartTime(session_id);
            overridden.has_value()) {
            return *overridden;
        }
    }
    return NowTimestamp();
}

isla::server::memory::Timestamp
GatewayStubResponder::ResolveConversationMessageTime(std::string_view session_id,
                                                     std::string_view turn_id,
                                                     isla::server::memory::MessageRole role) const {
    if (config_.session_clock != nullptr) {
        if (const std::optional<isla::server::memory::Timestamp> overridden =
                config_.session_clock->ResolveConversationMessageTime(session_id, turn_id, role);
            overridden.has_value()) {
            return *overridden;
        }
    }
    return NowTimestamp();
}

absl::StatusOr<isla::server::memory::UserQueryMemoryResult>
GatewayStubResponder::HandleAcceptedTurnMemory(const TurnAcceptedEvent& event) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(event.session_id);
    if (session_memory != nullptr) {
        std::lock_guard<std::mutex> lock(session_memory->mutex);
        absl::StatusOr<isla::server::memory::UserQueryMemoryResult> result =
            session_memory->orchestrator.HandleUserQuery(isla::server::memory::GatewayUserQuery(
                event.session_id, event.turn_id, event.text,
                ResolveConversationMessageTime(event.session_id, event.turn_id,
                                               isla::server::memory::MessageRole::User)));
        if (result.ok() && config_.on_user_query_memory_ready) {
            config_.on_user_query_memory_ready(event.session_id, *result);
        }
        return result;
    }

    if (const std::optional<absl::Status> start_failure = FindSessionStartFailure(event.session_id);
        start_failure.has_value()) {
        LOG(WARNING) << "AI gateway stub rejecting turn because session startup previously failed "
                     << "session=" << SanitizeForLog(event.session_id)
                     << " turn_id=" << SanitizeForLog(event.turn_id) << " detail='"
                     << SanitizeForLog(start_failure->message()) << "'";
        return *start_failure;
    }

    return absl::FailedPreconditionError("missing memory orchestrator for started session");
}

absl::Status GatewayStubResponder::HandleSuccessfulReplyMemory(const PendingTurn& turn,
                                                               std::string_view reply_text) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(turn.session_id);
    if (session_memory == nullptr) {
        return absl::FailedPreconditionError("missing memory orchestrator for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.HandleAssistantReply(
        isla::server::memory::GatewayAssistantReply(
            turn.session_id, turn.turn_id, std::string(reply_text),
            ResolveConversationMessageTime(turn.session_id, turn.turn_id,
                                           isla::server::memory::MessageRole::Assistant)));
}

absl::StatusOr<std::string>
GatewayStubResponder::ExpandMidTermEpisodeForSession(std::string_view session_id,
                                                     std::string_view episode_id) const {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory == nullptr) {
        return absl::NotFoundError("memory orchestrator not found for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.ExpandMidTermEpisode(episode_id);
}

std::optional<isla::server::tools::ToolExecutionContext>
GatewayStubResponder::BuildToolExecutionContext(
    std::string_view session_id,
    std::shared_ptr<const TurnTelemetryContext> telemetry_context) const {
    if (session_id.empty()) {
        return std::nullopt;
    }
    return isla::server::tools::ToolExecutionContext{
        .session_id = std::string(session_id),
        .telemetry_context = std::move(telemetry_context),
        .session_reader = std::make_shared<CallbackToolSessionReader>(
            [this, session_id = std::string(session_id)](
                std::string_view episode_id) -> absl::StatusOr<std::string> {
                return ExpandMidTermEpisodeForSession(session_id, episode_id);
            }),
    };
}

absl::StatusOr<std::string>
GatewayStubResponder::RenderSessionMemoryPrompt(std::string_view session_id) const {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory == nullptr) {
        return absl::NotFoundError("memory orchestrator not found for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.RenderFullWorkingMemory();
}

absl::StatusOr<isla::server::memory::WorkingMemoryState>
GatewayStubResponder::SnapshotSessionWorkingMemoryState(std::string_view session_id) const {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory == nullptr) {
        return absl::NotFoundError("memory orchestrator not found for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    return session_memory->orchestrator.memory().snapshot();
}

absl::Status GatewayStubResponder::AwaitSessionMemorySettled(std::string_view session_id) {
    const std::shared_ptr<SessionMemoryState> session_memory = FindSessionMemory(session_id);
    if (session_memory == nullptr) {
        return absl::NotFoundError("memory orchestrator not found for session");
    }
    std::lock_guard<std::mutex> lock(session_memory->mutex);
    const absl::StatusOr<std::size_t> drained =
        session_memory->orchestrator.AwaitAndDrainAllPendingMidTermCompactions();
    if (!drained.ok()) {
        return drained.status();
    }
    if (*drained > 0U) {
        VLOG(1) << "AI gateway stub memory settled session=" << SanitizeForLog(session_id)
                << " drained_count=" << *drained;
    }
    return absl::OkStatus();
}

bool GatewayStubResponder::WaitForAcceptedTurns(std::size_t expected_count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2),
                        [this, expected_count] { return accepted_turns_count_ >= expected_count; });
}

} // namespace isla::server::ai_gateway
