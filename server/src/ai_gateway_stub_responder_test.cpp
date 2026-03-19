#include "isla/server/ai_gateway_stub_responder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;

absl::Status EmitMidTermAwareEchoResponse(const OpenAiResponsesRequest& request,
                                          const OpenAiResponsesEventCallback& on_event,
                                          std::string_view prefix);

struct EmittedEvent {
    std::string op;
    std::string turn_id;
    std::string payload;
};

struct TelemetryEventRecord {
    std::string name;
};

struct TelemetryPhaseRecord {
    std::string name;
    TurnTelemetryContext::Clock::time_point started_at;
    TurnTelemetryContext::Clock::time_point completed_at;
};

struct TurnFinishedRecord {
    std::string outcome;
};

std::shared_ptr<test::FakeOpenAiResponsesClient>
MakeEchoOpenAiResponsesClient(std::string prefix = "stub echo: ") {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [prefix = std::move(prefix)](const OpenAiResponsesRequest& request,
                                     const OpenAiResponsesEventCallback& on_event) {
            return EmitMidTermAwareEchoResponse(request, on_event, prefix);
        });
}

absl::Status EmitResponseText(std::string_view text, const OpenAiResponsesEventCallback& on_event,
                              std::string_view response_id = "resp_test") {
    const absl::Status delta_status =
        on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = std::string(text) });
    if (!delta_status.ok()) {
        return delta_status;
    }
    return on_event(OpenAiResponsesCompletedEvent{
        .response_id = std::string(response_id),
    });
}

const std::string& MidTermFlushDeciderPromptText() {
    static const std::string prompt = [] {
        const absl::StatusOr<std::string> loaded = isla::server::memory::LoadPrompt(
            isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
        if (!loaded.ok()) {
            throw std::runtime_error("failed to load mid-term flush decider prompt");
        }
        return *loaded;
    }();
    return prompt;
}

const std::string& MidTermCompactorPromptText() {
    static const std::string prompt = [] {
        const absl::StatusOr<std::string> loaded = isla::server::memory::LoadPrompt(
            isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
        if (!loaded.ok()) {
            throw std::runtime_error("failed to load mid-term compactor prompt");
        }
        return *loaded;
    }();
    return prompt;
}

bool IsMidTermMemoryRequest(const OpenAiResponsesRequest& request) {
    return request.system_prompt == MidTermFlushDeciderPromptText() ||
           request.system_prompt == MidTermCompactorPromptText();
}

absl::Status EmitMidTermAwareEchoResponse(const OpenAiResponsesRequest& request,
                                          const OpenAiResponsesEventCallback& on_event,
                                          std::string_view prefix = "stub echo: ") {
    if (request.system_prompt == MidTermFlushDeciderPromptText()) {
        return EmitResponseText(R"json({
            "should_flush": false,
            "item_id": null,
            "split_at": null,
            "reasoning": "No completed episode boundary."
        })json",
                                on_event, "resp_decider");
    }
    if (request.system_prompt == MidTermCompactorPromptText()) {
        return EmitResponseText(R"json({
            "tier1_detail": "Fallback detail.",
            "tier2_summary": "Fallback summary.",
            "tier3_ref": "Fallback ref.",
            "tier3_keywords": ["fallback", "summary", "memory", "test", "compactor"],
            "salience": 5
        })json",
                                on_event, "resp_compactor");
    }

    const std::string text = std::string(prefix) + test::ExtractLatestPromptLine(request.user_text);
    const std::size_t midpoint = text.size() / 2U;
    const absl::Status first_status = on_event(OpenAiResponsesTextDeltaEvent{
        .text_delta = text.substr(0, midpoint),
    });
    if (!first_status.ok()) {
        return first_status;
    }
    const absl::Status second_status = on_event(OpenAiResponsesTextDeltaEvent{
        .text_delta = text.substr(midpoint),
    });
    if (!second_status.ok()) {
        return second_status;
    }
    return on_event(OpenAiResponsesCompletedEvent{
        .response_id = "resp_test",
    });
}

bool ContainsTelemetryName(const std::vector<TelemetryEventRecord>& events, std::string_view name) {
    return std::find_if(events.begin(), events.end(), [name](const TelemetryEventRecord& event) {
               return event.name == name;
           }) != events.end();
}

bool ContainsTelemetryName(const std::vector<TelemetryPhaseRecord>& phases, std::string_view name) {
    return std::find_if(phases.begin(), phases.end(), [name](const TelemetryPhaseRecord& phase) {
               return phase.name == name;
           }) != phases.end();
}

class RecordingTelemetrySink final : public TelemetrySink {
  public:
    void OnEvent(const TurnTelemetryContext& context, std::string_view event_name,
                 TurnTelemetryContext::Clock::time_point at) const override {
        static_cast<void>(context);
        static_cast<void>(at);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(TelemetryEventRecord{ .name = std::string(event_name) });
        }
        cv_.notify_all();
    }

    void OnPhase(const TurnTelemetryContext& context, std::string_view phase_name,
                 TurnTelemetryContext::Clock::time_point started_at,
                 TurnTelemetryContext::Clock::time_point completed_at) const override {
        static_cast<void>(context);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phases_.push_back(TelemetryPhaseRecord{
                .name = std::string(phase_name),
                .started_at = started_at,
                .completed_at = completed_at,
            });
        }
        cv_.notify_all();
    }

    void OnTurnFinished(const TurnTelemetryContext& context, std::string_view outcome,
                        TurnTelemetryContext::Clock::time_point finished_at) const override {
        static_cast<void>(context);
        static_cast<void>(finished_at);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_.push_back(TurnFinishedRecord{ .outcome = std::string(outcome) });
        }
        cv_.notify_all();
    }

    bool WaitForFinishedCount(std::size_t expected_count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] { return finished_.size() >= expected_count; });
    }

    [[nodiscard]] std::vector<TelemetryEventRecord> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    [[nodiscard]] std::vector<TelemetryPhaseRecord> phases() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phases_;
    }

    [[nodiscard]] std::vector<TurnFinishedRecord> finished() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable std::vector<TelemetryEventRecord> events_;
    mutable std::vector<TelemetryPhaseRecord> phases_;
    mutable std::vector<TurnFinishedRecord> finished_;
};

class RecordingLiveSession final : public GatewayLiveSession {
  public:
    explicit RecordingLiveSession(std::string session_id) : session_id_(std::move(session_id)) {}

    [[nodiscard]] const std::string& session_id() const override {
        return session_id_;
    }

    [[nodiscard]] bool is_closed() const override {
        return closed_;
    }

    void AsyncEmitTextOutput(std::string turn_id, std::string text,
                             GatewayEmitCallback on_complete) override {
        absl::Status status = absl::OkStatus();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fail_next_text_output_) {
                fail_next_text_output_ = false;
                status = absl::UnavailableError("text output failed");
            }
        }
        if (status.ok()) {
            RecordEvent({ .op = "text.output", .turn_id = std::move(turn_id), .payload = text });
        }
        on_complete(std::move(status));
    }

    void AsyncEmitAudioOutput(std::string turn_id, std::string mime_type, std::string audio_base64,
                              GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "audio.output",
            .turn_id = std::move(turn_id),
            .payload = mime_type + ":" + audio_base64,
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitTurnCompleted(std::string turn_id, GatewayEmitCallback on_complete) override {
        EmittedEvent event{
            .op = "turn.completed",
            .turn_id = std::move(turn_id),
            .payload = "",
        };
        RecordEvent(event);
        InvokeHook(event);
        on_complete(absl::OkStatus());
    }

    void AsyncEmitTurnCancelled(std::string turn_id, GatewayEmitCallback on_complete) override {
        RecordEvent({
            .op = "turn.cancelled",
            .turn_id = std::move(turn_id),
            .payload = "",
        });
        on_complete(absl::OkStatus());
    }

    void AsyncEmitError(std::optional<std::string> turn_id, std::string code, std::string message,
                        GatewayEmitCallback on_complete) override {
        absl::Status status = absl::OkStatus();
        bool delay_completion = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fail_next_error_output_) {
                fail_next_error_output_ = false;
                status = absl::UnavailableError("error output failed");
            }
            delay_completion = delay_next_error_completion_;
            if (delay_completion) {
                delay_next_error_completion_ = false;
                pending_error_completion_ = std::move(on_complete);
            }
        }
        if (status.ok()) {
            RecordEvent({
                .op = "error",
                .turn_id = turn_id.value_or(""),
                .payload = code + ":" + message,
            });
        }
        if (!delay_completion) {
            on_complete(std::move(status));
        }
    }

    bool WaitForEventCount(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [&] { return events_.size() >= count; });
    }

    [[nodiscard]] std::vector<EmittedEvent> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    void FailNextTextOutput() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_text_output_ = true;
    }

    void FailNextErrorOutput() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_error_output_ = true;
    }

    void MarkClosed() {
        closed_ = true;
    }

    void SetEventHook(std::function<void(const EmittedEvent&)> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        event_hook_ = std::move(hook);
    }

    void DelayNextErrorCompletion() {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_next_error_completion_ = true;
    }

    void ReleasePendingErrorCompletion(absl::Status status = absl::OkStatus()) {
        GatewayEmitCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = std::move(pending_error_completion_);
        }
        if (callback) {
            callback(std::move(status));
        }
    }

  private:
    void RecordEvent(EmittedEvent event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        cv_.notify_all();
    }

    void InvokeHook(const EmittedEvent& event) {
        std::function<void(const EmittedEvent&)> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = event_hook_;
        }
        if (hook) {
            hook(event);
        }
    }

    std::string session_id_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<EmittedEvent> events_;
    std::function<void(const EmittedEvent&)> event_hook_;
    GatewayEmitCallback pending_error_completion_;
    bool delay_next_error_completion_ = false;
    bool fail_next_text_output_ = false;
    bool fail_next_error_output_ = false;
    bool closed_ = false;
};

class ResponderRegistryAttachment {
  public:
    explicit ResponderRegistryAttachment(GatewayStubResponder& responder)
        : responder_(responder), registry_(&responder_) {
        responder_.AttachSessionRegistry(&registry_);
    }

    ~ResponderRegistryAttachment() {
        registry_.NotifyServerStopping();
        responder_.AttachSessionRegistry(nullptr);
    }

    ResponderRegistryAttachment(const ResponderRegistryAttachment&) = delete;
    ResponderRegistryAttachment& operator=(const ResponderRegistryAttachment&) = delete;

    [[nodiscard]] GatewaySessionRegistry& registry() {
        return registry_;
    }

  private:
    GatewayStubResponder& responder_;
    GatewaySessionRegistry registry_;
};

class GatewayStubResponderTest : public ::testing::Test {
  protected:
    GatewayStubResponderTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 20ms,
              .openai_client = MakeEchoOpenAiResponsesClient(),
          }),
          session_(std::make_shared<RecordingLiveSession>("srv_test")) {
        registry_attachment_ = std::make_unique<ResponderRegistryAttachment>(responder_);
        registry_attachment_->registry().RegisterSession(session_);
    }

    void SetUp() override {
        responder_.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    }

    GatewayStubResponder responder_;
    std::unique_ptr<ResponderRegistryAttachment> registry_attachment_;
    std::shared_ptr<RecordingLiveSession> session_;
};

class RecordingGatewayMemoryStore final : public isla::server::memory::MemoryStore {
  public:
    absl::Status UpsertSession(const isla::server::memory::MemorySessionRecord& record) override {
        ++upsert_session_attempts;
        if (next_upsert_session_status_index < upsert_session_statuses.size()) {
            const absl::Status status = upsert_session_statuses[next_upsert_session_status_index++];
            if (!status.ok()) {
                return status;
            }
        }
        session_records.push_back(record);
        return absl::OkStatus();
    }

    absl::Status AppendConversationMessage(
        const isla::server::memory::ConversationMessageWrite& write) override {
        message_writes.push_back(write);
        return absl::OkStatus();
    }

    absl::Status ReplaceConversationItemWithEpisodeStub(
        const isla::server::memory::EpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status SplitConversationItemWithEpisodeStub(
        const isla::server::memory::SplitEpisodeStubWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::Status
    UpsertMidTermEpisode(const isla::server::memory::MidTermEpisodeWrite& write) override {
        static_cast<void>(write);
        return absl::OkStatus();
    }

    absl::StatusOr<std::vector<isla::server::memory::Episode>>
    ListMidTermEpisodes(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::vector<isla::server::memory::Episode>{};
    }

    absl::StatusOr<std::optional<isla::server::memory::Episode>>
    GetMidTermEpisode(std::string_view session_id, std::string_view episode_id) const override {
        static_cast<void>(session_id);
        static_cast<void>(episode_id);
        return std::nullopt;
    }

    absl::StatusOr<std::optional<isla::server::memory::MemoryStoreSnapshot>>
    LoadSnapshot(std::string_view session_id) const override {
        static_cast<void>(session_id);
        return std::nullopt;
    }

    std::vector<isla::server::memory::MemorySessionRecord> session_records;
    std::vector<isla::server::memory::ConversationMessageWrite> message_writes;
    std::vector<absl::Status> upsert_session_statuses;
    std::size_t next_upsert_session_status_index = 0;
    std::size_t upsert_session_attempts = 0;
};

TEST_F(GatewayStubResponderTest, SessionStartCreatesMemoryPromptBeforeAnyTurn) {
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("<conversation>"), std::string::npos);
    EXPECT_NE(prompt->find("- (empty)"), std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest, SessionStartPersistsSessionBeforeFirstTurn) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    ASSERT_EQ(store->session_records.size(), 1U);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    EXPECT_EQ(store->session_records.size(), 1U);
    ASSERT_EQ(store->message_writes.size(), 2U);
    EXPECT_EQ(store->message_writes[0].content, "hello");
    EXPECT_EQ(store->message_writes[1].content, "stub echo: hello");
}

TEST(GatewayStubResponderStandaloneTest, SessionStartRetriesTransientPersistenceFailures) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::UnavailableError("supabase unavailable"),
        absl::DeadlineExceededError("supabase timeout"),
    };
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .session_start_persistence_max_attempts = 3,
        .session_start_persistence_retry_delay = 0ms,
        .memory_user_id = "gateway_user",
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    EXPECT_EQ(store->upsert_session_attempts, 3U);
    ASSERT_EQ(store->session_records.size(), 1U);
    EXPECT_TRUE(session->events().empty());
}

TEST(GatewayStubResponderStandaloneTest, SessionStartDoesNotRetryNonRetryableFailures) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::PermissionDeniedError("supabase denied"),
        absl::UnavailableError("should not be consumed"),
    };
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .session_start_persistence_max_attempts = 3,
        .session_start_persistence_retry_delay = 0ms,
        .memory_user_id = "gateway_user",
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    ASSERT_TRUE(session->WaitForEventCount(1U));
    EXPECT_EQ(store->upsert_session_attempts, 1U);
    ASSERT_TRUE(store->session_records.empty());

    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "");
    EXPECT_EQ(events[0].payload, "internal_error:failed to initialize session memory");
}

TEST(GatewayStubResponderStandaloneTest, DuplicateSessionStartDoesNotPoisonHealthySession) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    ASSERT_TRUE(session->WaitForEventCount(1U));
    const std::vector<EmittedEvent> startup_events = session->events();
    ASSERT_EQ(startup_events.size(), 1U);
    EXPECT_EQ(startup_events[0].op, "error");
    EXPECT_EQ(startup_events[0].payload, "internal_error:failed to initialize session memory");

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(3U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1].op, "text.output");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[1].payload, "stub echo: hello");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest,
     SessionStartExhaustedRetriesEmitsErrorAndDoesNotRetryOnFirstTurn) {
    auto store = std::make_shared<RecordingGatewayMemoryStore>();
    store->upsert_session_statuses = {
        absl::UnavailableError("supabase unavailable"),
        absl::UnavailableError("supabase still unavailable"),
        absl::UnavailableError("supabase unavailable again"),
    };
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .session_start_persistence_max_attempts = 3,
        .session_start_persistence_retry_delay = 0ms,
        .memory_user_id = "gateway_user",
        .memory_store = store,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    ASSERT_TRUE(session->WaitForEventCount(1U));
    EXPECT_EQ(store->upsert_session_attempts, 3U);
    ASSERT_EQ(store->session_records.size(), 0U);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(3U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "");
    EXPECT_EQ(events[0].payload, "service_unavailable:failed to initialize session memory");
    EXPECT_EQ(events[1].op, "error");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[1].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_1");
    EXPECT_EQ(store->upsert_session_attempts, 3U);
}

TEST_F(GatewayStubResponderTest, AcceptedTurnProvidesRenderedPromptPiecesToOpenAiClient) {
    auto capturing_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "reply", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request,
           const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (IsMidTermMemoryRequest(request)) {
                return EmitMidTermAwareEchoResponse(request, on_event);
            }
            return EmitResponseText("reply", on_event);
        });

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = capturing_client,
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    const std::shared_ptr<const TurnTelemetryContext> telemetry_context =
        MakeTurnTelemetryContext("srv_test", "turn_1");

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
        .telemetry_context = telemetry_context,
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<OpenAiResponsesRequest> requests = capturing_client->requests_snapshot();
    auto main_request = std::find_if(
        requests.rbegin(), requests.rend(),
        [](const OpenAiResponsesRequest& request) { return !IsMidTermMemoryRequest(request); });
    ASSERT_NE(main_request, requests.rend());
    EXPECT_NE(main_request->system_prompt.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("- [user | "), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("- [assistant | "), std::string::npos);
    EXPECT_EQ(main_request->system_prompt.find("] hello"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<conversation>"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<mid_term_episodes>"), std::string::npos);
    EXPECT_NE(main_request->user_text.find("<retrieved_memory>"), std::string::npos);
    EXPECT_EQ(main_request->user_text.find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_EQ(main_request->user_text.find("You are Isla."), std::string::npos);
    EXPECT_NE(main_request->user_text.find("] hello"), std::string::npos);
    EXPECT_EQ(main_request->telemetry_context, telemetry_context);
}

TEST(GatewayStubResponderStandaloneTest,
     ExtractLatestPromptLinePreservesBracketSpaceInsideConversationContent) {
    EXPECT_EQ(test::ExtractLatestPromptLine(
                  "<mid_term_episodes>\n- (none)\n<retrieved_memory>\n(none)\n<conversation>\n"
                  "- [user | 2026-03-14T20:00:00.000Z] hello ] world\n"),
              "hello ] world");
}

TEST_F(GatewayStubResponderTest,
       SecondTurnRequestIncludesPriorConversationWithoutDuplicatingCurrentTurn) {
    auto capturing_client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [](const OpenAiResponsesRequest& request, const OpenAiResponsesEventCallback& on_event)
            -> absl::Status { return EmitMidTermAwareEchoResponse(request, on_event); });

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = capturing_client,
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    ASSERT_TRUE(session->WaitForEventCount(2U));

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "how are you?",
    });
    ASSERT_TRUE(session->WaitForEventCount(4U));

    const std::vector<OpenAiResponsesRequest> requests = capturing_client->requests_snapshot();
    auto second_request = std::find_if(
        requests.rbegin(), requests.rend(),
        [](const OpenAiResponsesRequest& request) { return !IsMidTermMemoryRequest(request); });
    ASSERT_NE(second_request, requests.rend());
    const std::string& second_request_context = second_request->user_text;
    EXPECT_NE(second_request_context.find("<conversation>"), std::string::npos);
    EXPECT_NE(second_request_context.find("] hello"), std::string::npos);
    EXPECT_NE(second_request_context.find("] stub echo: hello"), std::string::npos);
    EXPECT_NE(second_request_context.find("] how are you?"), std::string::npos);

    const std::size_t current_turn_pos = second_request_context.find("] how are you?");
    ASSERT_NE(current_turn_pos, std::string::npos);
    EXPECT_EQ(second_request_context.find("] how are you?", current_turn_pos + 1U),
              std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest, MidTermMemoryWiringEventuallyFlushesCompletedTurnIntoLaterPrompt) {
    const absl::StatusOr<std::string> decider_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermFlushDeciderSystemPrompt);
    const absl::StatusOr<std::string> compactor_prompt = isla::server::memory::LoadPrompt(
        isla::server::memory::PromptAsset::kMidTermCompactorSystemPrompt);
    ASSERT_TRUE(decider_prompt.ok()) << decider_prompt.status();
    ASSERT_TRUE(compactor_prompt.ok()) << compactor_prompt.status();

    auto recorded_requests = std::make_shared<std::vector<OpenAiResponsesRequest>>();
    auto requests_mutex = std::make_shared<std::mutex>();
    auto compactor_finished = std::make_shared<std::promise<void>>();
    std::future<void> compactor_finished_future = compactor_finished->get_future();
    auto compactor_finished_once = std::make_shared<std::once_flag>();
    auto decider_call_count = std::make_shared<int>(0);
    auto client = test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [recorded_requests, requests_mutex, decider_prompt = *decider_prompt,
         compactor_prompt = *compactor_prompt, compactor_finished, compactor_finished_once,
         decider_call_count](const OpenAiResponsesRequest& request,
                             const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            {
                std::lock_guard<std::mutex> lock(*requests_mutex);
                recorded_requests->push_back(request);
            }
            if (request.system_prompt == decider_prompt) {
                const int call_index = (*decider_call_count)++;
                if (call_index == 0) {
                    return EmitResponseText(R"json({
                        "should_flush": true,
                        "item_id": "i0",
                        "split_at": null,
                        "reasoning": "Completed first exchange."
                    })json",
                                            on_event, "resp_decider");
                }
                return EmitResponseText(R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No additional completed episode."
                })json",
                                        on_event, "resp_decider");
            }
            if (request.system_prompt == compactor_prompt) {
                const absl::Status status = EmitResponseText(R"json({
                    "tier1_detail": "First exchange detail.",
                    "tier2_summary": "First exchange summary.",
                    "tier3_ref": "First exchange ref.",
                    "tier3_keywords": ["first", "exchange", "summary", "memory", "test"],
                    "salience": 8
                })json",
                                                             on_event, "resp_compactor");
                if (status.ok()) {
                    std::call_once(*compactor_finished_once,
                                   [compactor_finished] { compactor_finished->set_value(); });
                }
                return status;
            }

            const std::string reply =
                std::string("stub echo: ") + test::ExtractLatestPromptLine(request.user_text);
            return EmitResponseText(reply, on_event);
        });

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .openai_client = client,
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    ASSERT_TRUE(session->WaitForEventCount(2U));
    ASSERT_EQ(compactor_finished_future.wait_for(2s), std::future_status::ready);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "follow up",
    });
    ASSERT_TRUE(session->WaitForEventCount(4U));

    auto prompt_contains_mid_term_summary = [&]() -> bool {
        const absl::StatusOr<std::string> prompt = responder.RenderSessionMemoryPrompt("srv_test");
        if (!prompt.ok()) {
            return false;
        }
        return prompt->find("First exchange summary.") != std::string::npos &&
               prompt->find("First exchange ref.") != std::string::npos;
    };

    std::size_t expected_event_count = 4U;
    int turn_number = 3;
    while (turn_number <= 8 && !prompt_contains_mid_term_summary()) {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_" + std::to_string(turn_number),
            .text = turn_number == 3 ? "third turn" : "later turn " + std::to_string(turn_number),
        });
        expected_event_count += 2U;
        ASSERT_TRUE(session->WaitForEventCount(expected_event_count));
        ++turn_number;
    }
    ASSERT_TRUE(prompt_contains_mid_term_summary());

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_visibility_probe",
        .text = "visibility probe",
    });
    expected_event_count += 2U;
    ASSERT_TRUE(session->WaitForEventCount(expected_event_count));

    std::vector<OpenAiResponsesRequest> requests;
    {
        std::lock_guard<std::mutex> lock(*requests_mutex);
        requests = *recorded_requests;
    }

    const auto decider_request = std::find_if(
        requests.begin(), requests.end(), [&decider_prompt](const OpenAiResponsesRequest& request) {
            return request.system_prompt == *decider_prompt;
        });
    ASSERT_NE(decider_request, requests.end());
    EXPECT_EQ(decider_request->model, kDefaultMidTermMemoryModel);

    const auto compactor_request =
        std::find_if(requests.begin(), requests.end(),
                     [&compactor_prompt](const OpenAiResponsesRequest& request) {
                         return request.system_prompt == *compactor_prompt;
                     });
    ASSERT_NE(compactor_request, requests.end());
    EXPECT_EQ(compactor_request->model, kDefaultMidTermMemoryModel);

    const auto later_reply_request =
        std::find_if(requests.begin(), requests.end(),
                     [&decider_prompt, &compactor_prompt](const OpenAiResponsesRequest& request) {
                         return request.system_prompt != *decider_prompt &&
                                request.system_prompt != *compactor_prompt &&
                                request.user_text.find("ep_srv_test_1") != std::string::npos;
                     });
    ASSERT_NE(later_reply_request, requests.end());
    EXPECT_NE(later_reply_request->user_text.find("<mid_term_episodes>"), std::string::npos);
    EXPECT_EQ(later_reply_request->user_text.find("<mid_term_episodes>\n- (none)"),
              std::string::npos);
    EXPECT_NE(later_reply_request->user_text.find("ep_srv_test_1"), std::string::npos);

    const absl::StatusOr<std::string> prompt = responder.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("First exchange summary."), std::string::npos);
    EXPECT_NE(prompt->find("First exchange ref."), std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest,
     MidTermMemoryNotConfiguredStillAllowsSessionMemoryStartup) {

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
    });
    EXPECT_FALSE(responder.IsMidTermMemoryConfigured());
    EXPECT_FALSE(responder.IsMidTermMemoryAvailable());
    EXPECT_TRUE(responder.MidTermMemoryInitializationStatus().ok());
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    ASSERT_TRUE(responder.RenderSessionMemoryPrompt("srv_test").ok());
    EXPECT_TRUE(session->events().empty());
}

TEST_F(GatewayStubResponderTest, SessionStartUsesBundledSystemPromptByDefault) {
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    const absl::StatusOr<std::string> system_prompt = isla::server::memory::LoadSystemPrompt();

    ASSERT_TRUE(prompt.ok()) << prompt.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_EQ(prompt->compare(0, system_prompt->size(), *system_prompt), 0);
}

TEST_F(GatewayStubResponderTest, AcceptedTurnEmitsStubTextAndCompletion) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, SuccessfulTurnEmitsPhaseTwoTelemetrySlices) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_success",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_success", telemetry_sink),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventMemoryUserQueryStarted,
        telemetry::kEventMemoryUserQueryCompleted,
        telemetry::kEventTurnEnqueued,
        telemetry::kEventTurnDequeued,
        telemetry::kEventPlanCreateStarted,
        telemetry::kEventPlanCreateCompleted,
        telemetry::kEventExecutorStarted,
        telemetry::kEventExecutorCompleted,
        telemetry::kEventTextOutputEmitStarted,
        telemetry::kEventTextOutputEmitCompleted,
        telemetry::kEventMemoryAssistantReplyStarted,
        telemetry::kEventMemoryAssistantReplyCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }

    const std::array expected_phase_names = {
        telemetry::kPhaseMemoryUserQuery,   telemetry::kPhaseQueueWait,
        telemetry::kPhasePlanCreate,        telemetry::kPhaseExecutorTotal,
        telemetry::kPhaseEmitTextOutput,    telemetry::kPhaseMemoryAssistantReply,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    for (const TelemetryPhaseRecord& phase : phases) {
        EXPECT_LE(phase.started_at, phase.completed_at);
    }

    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeSucceeded);
}

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnFlowsThroughPlannerAndExecutorBoundary) {
    std::optional<ExecutionPlan> execution_plan;

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_execution_plan = [&](const ExecutionPlan& plan) { execution_plan = plan; },
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    ASSERT_TRUE(execution_plan.has_value());
    ASSERT_EQ(execution_plan->steps.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<OpenAiLlmStep>(execution_plan->steps.front()));
    const OpenAiLlmStep& openai_step = std::get<OpenAiLlmStep>(execution_plan->steps.front());
    EXPECT_EQ(openai_step.step_name, "main");
    EXPECT_EQ(openai_step.model, "gpt-5.3-chat-latest");
    EXPECT_EQ(openai_step.reasoning_effort, OpenAiReasoningEffort::kMedium);

    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
}

TEST(GatewayStubResponderStandaloneTest, MissingSessionMemoryStillEmitsFailureTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_missing_memory",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_missing_memory", telemetry_sink),
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<EmittedEvent> emitted = session->events();
    ASSERT_EQ(emitted.size(), 2U);
    EXPECT_EQ(emitted[0].op, "error");
    EXPECT_EQ(emitted[0].turn_id, "turn_missing_memory");
    EXPECT_EQ(emitted[1].op, "turn.completed");

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventMemoryUserQueryStarted,
        telemetry::kEventMemoryUserQueryCompleted,
        telemetry::kEventTurnFailed,
        telemetry::kEventErrorEmitStarted,
        telemetry::kEventErrorEmitCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseMemoryUserQuery,
        telemetry::kPhaseEmitError,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeFailed);
}

TEST_F(GatewayStubResponderTest, AcceptedTurnUpdatesSessionMemoryPrompt) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- [user | "), std::string::npos);
    EXPECT_NE(prompt->find("] hello"), std::string::npos);
    EXPECT_NE(prompt->find("- [assistant | "), std::string::npos);
    EXPECT_NE(prompt->find("] stub echo: hello"), std::string::npos);
}

TEST_F(GatewayStubResponderTest, CancelBeforeReplyEmitsTurnCancelled) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.cancelled");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, CancelledTurnEmitsCancellationTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_cancel",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_cancel", telemetry_sink),
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_cancel",
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventTurnCancelledEmitStarted,
        telemetry::kEventTurnCancelledEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseEmitTurnCancelled,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeCancelled);
}

TEST_F(GatewayStubResponderTest, MismatchedCancelRequestDoesNotAffectTrackedTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });
    responder_.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_other",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, ServerStoppingEmitsErrorThenCompletionForPendingTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    responder_.OnServerStopping(registry_attachment_->registry());

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "server_stopping:server stopping");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, SessionClosedBeforeReplyDropsPendingTurn) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    session_->MarkClosed();
    registry_attachment_->registry().OnSessionClosed(SessionClosedEvent{
        .session_id = "srv_test",
        .session_started = true,
        .inflight_turn_id = std::string("turn_1"),
        .reason = SessionCloseReason::TransportClosed,
        .detail = "client closed",
    });

    std::this_thread::sleep_for(60ms);
    EXPECT_TRUE(session_->events().empty());
}

TEST(GatewayStubResponderStandaloneTest, SessionClosedDuringExecutionDropsLaterEmits) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
            absl::OkStatus(), "", "resp_test", absl::OkStatus(),
            [builder_started,
             allow_finish_future](const OpenAiResponsesRequest& request,
                                  const OpenAiResponsesEventCallback& on_event) -> absl::Status {
                if (IsMidTermMemoryRequest(request)) {
                    return EmitMidTermAwareEchoResponse(request, on_event);
                }
                builder_started->set_value();
                allow_finish_future.wait();
                const std::string text = std::string("stub echo: ") + request.user_text;
                const absl::Status delta_status =
                    on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
                if (!delta_status.ok()) {
                    return delta_status;
                }
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_test",
                });
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    session->MarkClosed();
    registry.OnSessionClosed(SessionClosedEvent{
        .session_id = "srv_test",
        .session_started = true,
        .inflight_turn_id = std::string("turn_1"),
        .reason = SessionCloseReason::TransportClosed,
        .detail = "client closed",
    });
    allow_finish->set_value();

    std::this_thread::sleep_for(60ms);
    EXPECT_TRUE(session->events().empty());
}

TEST_F(GatewayStubResponderTest, EmitFailureDoesNotBlockLaterTurns) {
    session_->FailNextTextOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "first",
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    {
        const std::vector<EmittedEvent> first_events = session_->events();
        ASSERT_EQ(first_events.size(), 2U);
        EXPECT_EQ(first_events[0].op, "error");
        EXPECT_EQ(first_events[0].turn_id, "turn_1");
        EXPECT_EQ(first_events[0].payload,
                  "internal_error:stub responder failed to emit text output");
        EXPECT_EQ(first_events[1].op, "turn.completed");
        EXPECT_EQ(first_events[1].turn_id, "turn_1");
    }

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "second",
    });

    ASSERT_TRUE(session_->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: second");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST_F(GatewayStubResponderTest, FailedTurnEmitsFailureTelemetry) {
    auto telemetry_sink = std::make_shared<RecordingTelemetrySink>();
    session_->FailNextTextOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_telemetry_failure",
        .text = "hello",
        .telemetry_context =
            MakeTurnTelemetryContext("srv_test", "turn_telemetry_failure", telemetry_sink),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    ASSERT_TRUE(telemetry_sink->WaitForFinishedCount(1U));

    const std::vector<TelemetryEventRecord> events = telemetry_sink->events();
    const std::vector<TelemetryPhaseRecord> phases = telemetry_sink->phases();
    const std::vector<TurnFinishedRecord> finished = telemetry_sink->finished();

    const std::array expected_event_names = {
        telemetry::kEventTurnFailed,
        telemetry::kEventErrorEmitStarted,
        telemetry::kEventErrorEmitCompleted,
        telemetry::kEventTurnCompletedEmitStarted,
        telemetry::kEventTurnCompletedEmitCompleted,
    };
    for (std::string_view name : expected_event_names) {
        EXPECT_TRUE(ContainsTelemetryName(events, name)) << name;
    }
    const std::array expected_phase_names = {
        telemetry::kPhaseEmitError,
        telemetry::kPhaseEmitTurnCompleted,
    };
    for (std::string_view name : expected_phase_names) {
        EXPECT_TRUE(ContainsTelemetryName(phases, name)) << name;
    }
    ASSERT_EQ(finished.size(), 1U);
    EXPECT_EQ(finished.front().outcome, telemetry::kOutcomeFailed);
}

TEST_F(GatewayStubResponderTest, CompletionHookCanQueueNextTurnForSameSession) {
    session_->SetEventHook([this](const EmittedEvent& event) {
        if (event.op == "turn.completed" && event.turn_id == "turn_1") {
            responder_.OnTurnAccepted(TurnAcceptedEvent{
                .session_id = "srv_test",
                .turn_id = "turn_2",
                .text = "second",
            });
        }
    });

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "first",
    });

    ASSERT_TRUE(session_->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: second");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST_F(GatewayStubResponderTest, OversizedAcceptedTurnIsTerminalizedWithoutReply) {
    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = std::string(kMaxTextInputBytes + 1U, 'x'),
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "bad_request:text.input text exceeds maximum length");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, OversizedTurnStillCompletesWhenErrorEmitFails) {
    session_->FailNextErrorOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = std::string(kMaxTextInputBytes + 1U, 'x'),
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.completed");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST_F(GatewayStubResponderTest, OversizedRenderedWorkingMemoryContextIsTerminalizedWithoutReply) {
    const std::string large_turn_text(24U * 1024U, 'x');

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = large_turn_text,
    });

    ASSERT_TRUE(session_->WaitForEventCount(2U));

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = large_turn_text,
    });

    ASSERT_TRUE(session_->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, "error");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload,
              "bad_request:rendered working memory context exceeds maximum length");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST(GatewayStubResponderStandaloneTest, ReplyBuilderExceptionTerminatesTurnAndWorkerContinues) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
            absl::OkStatus(), "", "resp_test", absl::OkStatus(),
            [](const OpenAiResponsesRequest& request,
               const OpenAiResponsesEventCallback& on_event) -> absl::Status {
                if (IsMidTermMemoryRequest(request)) {
                    return EmitMidTermAwareEchoResponse(request, on_event);
                }
                const std::string latest_text = test::ExtractLatestPromptLine(request.user_text);
                if (latest_text == "explode") {
                    throw std::runtime_error("synthetic reply builder failure");
                }
                const std::string text = std::string("stub echo: ") + latest_text;
                const absl::Status delta_status =
                    on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
                if (!delta_status.ok()) {
                    return delta_status;
                }
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_test",
                });
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "explode",
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    {
        const std::vector<EmittedEvent> events = session->events();
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[0].op, "error");
        EXPECT_EQ(events[0].turn_id, "turn_1");
        EXPECT_EQ(events[0].payload, "internal_error:execution step failed");
        EXPECT_EQ(events[1].op, "turn.completed");
        EXPECT_EQ(events[1].turn_id, "turn_1");
    }

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "ok",
    });

    ASSERT_TRUE(session->WaitForEventCount(4U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[2].op, "text.output");
    EXPECT_EQ(events[2].turn_id, "turn_2");
    EXPECT_EQ(events[2].payload, "stub echo: ok");
    EXPECT_EQ(events[3].op, "turn.completed");
    EXPECT_EQ(events[3].turn_id, "turn_2");
}

TEST(GatewayStubResponderStandaloneTest, OpenAiProviderFailureEmitsMappedErrorAndCompletion) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client =
            test::MakeFakeOpenAiResponsesClient(absl::UnavailableError("provider down")),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "service_unavailable:upstream service unavailable");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnWithoutSessionStartFailsClosed) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "error");
    EXPECT_EQ(events[0].turn_id, "turn_1");
    EXPECT_EQ(events[0].payload, "internal_error:stub responder failed to update memory");
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest, MatchingCancelForInProgressTurnEmitsCancelled) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
            absl::OkStatus(), "", "resp_test", absl::OkStatus(),
            [builder_started,
             allow_finish_future](const OpenAiResponsesRequest& request,
                                  const OpenAiResponsesEventCallback& on_event) -> absl::Status {
                if (IsMidTermMemoryRequest(request)) {
                    return EmitMidTermAwareEchoResponse(request, on_event);
                }
                builder_started->set_value();
                allow_finish_future.wait();
                const std::string text = std::string("stub echo: ") + request.user_text;
                const absl::Status delta_status =
                    on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = text });
                if (!delta_status.ok()) {
                    return delta_status;
                }
                return on_event(OpenAiResponsesCompletedEvent{
                    .response_id = "resp_test",
                });
            }),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "hello",
    });

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    responder.OnTurnCancelRequested(TurnCancelRequestedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
    });
    allow_finish->set_value();

    ASSERT_TRUE(session->WaitForEventCount(1U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].op, "turn.cancelled");
    EXPECT_EQ(events[0].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest, SessionCloseAfterSessionStartRemovesEmptyMemory) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    ASSERT_TRUE(responder.RenderSessionMemoryPrompt("srv_test").ok());

    responder.OnSessionClosed(SessionClosedEvent{
        .session_id = "srv_test",
        .session_started = true,
        .inflight_turn_id = std::nullopt,
        .reason = SessionCloseReason::ProtocolEnded,
        .detail = "session ended",
    });

    const absl::StatusOr<std::string> prompt = responder.RenderSessionMemoryPrompt("srv_test");
    EXPECT_FALSE(prompt.ok());
    EXPECT_EQ(prompt.status().code(), absl::StatusCode::kNotFound);
}

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnDuringShutdownDoesNotBlockOnDelayedEmit) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    responder.OnServerStopping(registry);
    session->DelayNextErrorCompletion();

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_1",
            .text = "hello",
        });
    });

    EXPECT_EQ(accepted_future.wait_for(100ms), std::future_status::ready);
    ASSERT_TRUE(session->WaitForEventCount(1U));
    {
        const std::vector<EmittedEvent> events = session->events();
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events[0].op, "error");
        EXPECT_EQ(events[0].turn_id, "turn_1");
        EXPECT_EQ(events[0].payload, "server_stopping:server stopping");
    }

    session->ReleasePendingErrorCompletion();
    ASSERT_TRUE(session->WaitForEventCount(2U));
    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1].op, "turn.completed");
    EXPECT_EQ(events[1].turn_id, "turn_1");
}

TEST(GatewayStubResponderStandaloneTest,
     DifferentSessionRenderDoesNotBlockWhileOtherSessionMemoryIsLocked) {
    auto user_query_started = std::make_shared<std::promise<void>>();
    std::future<void> user_query_started_future = user_query_started->get_future();
    auto allow_user_query_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_user_query_finish_future =
        allow_user_query_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_user_query_memory_ready =
            [user_query_started, allow_user_query_finish_future](
                std::string_view session_id,
                const isla::server::memory::UserQueryMemoryResult& user_query_memory_result) {
                static_cast<void>(user_query_memory_result);
                if (session_id != "srv_one") {
                    return;
                }
                user_query_started->set_value();
                allow_user_query_finish_future.wait();
            },
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    registry.RegisterSession(session_one);
    registry.RegisterSession(session_two);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_one" });
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_two" });

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_one",
            .turn_id = "turn_1",
            .text = "hello from one",
        });
    });

    ASSERT_EQ(user_query_started_future.wait_for(2s), std::future_status::ready);

    auto render_future = std::async(std::launch::async,
                                    [&] { return responder.RenderSessionMemoryPrompt("srv_two"); });
    ASSERT_EQ(render_future.wait_for(100ms), std::future_status::ready);
    const absl::StatusOr<std::string> prompt = render_future.get();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- (empty)"), std::string::npos);

    allow_user_query_finish->set_value();
    ASSERT_EQ(accepted_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session_one->WaitForEventCount(2U));
}

TEST(GatewayStubResponderStandaloneTest, SameSessionRenderWaitsForOngoingMemoryMutation) {
    auto user_query_started = std::make_shared<std::promise<void>>();
    std::future<void> user_query_started_future = user_query_started->get_future();
    auto allow_user_query_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_user_query_finish_future =
        allow_user_query_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_user_query_memory_ready =
            [user_query_started, allow_user_query_finish_future](
                std::string_view session_id,
                const isla::server::memory::UserQueryMemoryResult& user_query_memory_result) {
                static_cast<void>(user_query_memory_result);
                if (session_id != "srv_test") {
                    return;
                }
                user_query_started->set_value();
                allow_user_query_finish_future.wait();
            },
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    registry.RegisterSession(session);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });

    auto accepted_future = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_test",
            .turn_id = "turn_1",
            .text = "hello",
        });
    });

    ASSERT_EQ(user_query_started_future.wait_for(2s), std::future_status::ready);

    auto render_future = std::async(
        std::launch::async, [&] { return responder.RenderSessionMemoryPrompt("srv_test"); });
    EXPECT_NE(render_future.wait_for(100ms), std::future_status::ready);

    allow_user_query_finish->set_value();
    ASSERT_EQ(accepted_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(render_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session->WaitForEventCount(2U));
    const absl::StatusOr<std::string> prompt = render_future.get();
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("- [user | "), std::string::npos);
    EXPECT_NE(prompt->find("] hello"), std::string::npos);
}

TEST(GatewayStubResponderStandaloneTest, ConcurrentMultiSessionTurnsKeepMemoryIsolated) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = MakeEchoOpenAiResponsesClient(),
    });
    ResponderRegistryAttachment registry_scope(responder);
    GatewaySessionRegistry& registry = registry_scope.registry();
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    registry.RegisterSession(session_one);
    registry.RegisterSession(session_two);
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_one" });
    responder.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_two" });

    auto first_turn = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_one",
            .turn_id = "turn_1",
            .text = "alpha",
        });
    });
    auto second_turn = std::async(std::launch::async, [&] {
        responder.OnTurnAccepted(TurnAcceptedEvent{
            .session_id = "srv_two",
            .turn_id = "turn_2",
            .text = "beta",
        });
    });

    ASSERT_EQ(first_turn.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second_turn.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(session_one->WaitForEventCount(2U));
    ASSERT_TRUE(session_two->WaitForEventCount(2U));

    const absl::StatusOr<std::string> prompt_one = responder.RenderSessionMemoryPrompt("srv_one");
    const absl::StatusOr<std::string> prompt_two = responder.RenderSessionMemoryPrompt("srv_two");
    ASSERT_TRUE(prompt_one.ok()) << prompt_one.status();
    ASSERT_TRUE(prompt_two.ok()) << prompt_two.status();
    EXPECT_NE(prompt_one->find("alpha"), std::string::npos);
    EXPECT_NE(prompt_one->find("stub echo: alpha"), std::string::npos);
    EXPECT_EQ(prompt_one->find("beta"), std::string::npos);
    EXPECT_NE(prompt_two->find("beta"), std::string::npos);
    EXPECT_NE(prompt_two->find("stub echo: beta"), std::string::npos);
    EXPECT_EQ(prompt_two->find("alpha"), std::string::npos);
}

} // namespace
} // namespace isla::server::ai_gateway
