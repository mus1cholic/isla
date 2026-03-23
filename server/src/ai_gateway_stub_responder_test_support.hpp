#ifndef ISLA_SERVER_SRC_AI_GATEWAY_STUB_RESPONDER_TEST_SUPPORT_HPP_
#define ISLA_SERVER_SRC_AI_GATEWAY_STUB_RESPONDER_TEST_SUPPORT_HPP_

#include "isla/server/ai_gateway_stub_responder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ai_gateway_test_mocks.hpp"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/memory/memory_store.hpp"
#include "isla/server/memory/prompt_loader.hpp"
#include "openai_responses_test_utils.hpp"
#include "server/memory/src/memory_store_mock.hpp"

namespace isla::server::ai_gateway::test_support {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

inline absl::Status EmitMidTermAwareEchoResponse(const OpenAiResponsesRequest& request,
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

inline std::shared_ptr<test::FakeOpenAiResponsesClient>
MakeEchoOpenAiResponsesClient(std::string prefix = "stub echo: ") {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [prefix = std::move(prefix)](const OpenAiResponsesRequest& request,
                                     const OpenAiResponsesEventCallback& on_event) {
            return EmitMidTermAwareEchoResponse(request, on_event, prefix);
        });
}

inline absl::Status EmitResponseText(std::string_view text,
                                     const OpenAiResponsesEventCallback& on_event,
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

inline const std::string& MidTermFlushDeciderPromptText() {
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

inline const std::string& MidTermCompactorPromptText() {
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

inline bool IsMidTermMemoryRequest(const OpenAiResponsesRequest& request) {
    return request.system_prompt == MidTermFlushDeciderPromptText() ||
           request.system_prompt == MidTermCompactorPromptText();
}

inline absl::Status EmitMidTermAwareEchoResponse(const OpenAiResponsesRequest& request,
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

inline bool ContainsTelemetryName(const std::vector<TelemetryEventRecord>& events,
                                  std::string_view name) {
    return std::find_if(events.begin(), events.end(), [name](const TelemetryEventRecord& event) {
               return event.name == name;
           }) != events.end();
}

inline bool ContainsTelemetryName(const std::vector<TelemetryPhaseRecord>& phases,
                                  std::string_view name) {
    return std::find_if(phases.begin(), phases.end(), [name](const TelemetryPhaseRecord& phase) {
               return phase.name == name;
           }) != phases.end();
}

class RecordingTelemetrySink final : public NiceMock<test::MockTelemetrySink> {
  public:
    RecordingTelemetrySink() {
        ON_CALL(*this, OnEvent(_, _, _))
            .WillByDefault([this](const TurnTelemetryContext& context, std::string_view event_name,
                                  TurnTelemetryContext::Clock::time_point at) {
                static_cast<void>(context);
                static_cast<void>(at);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    events_.push_back(TelemetryEventRecord{ .name = std::string(event_name) });
                }
                cv_.notify_all();
            });
        ON_CALL(*this, OnPhase(_, _, _, _))
            .WillByDefault([this](const TurnTelemetryContext& context, std::string_view phase_name,
                                  TurnTelemetryContext::Clock::time_point started_at,
                                  TurnTelemetryContext::Clock::time_point completed_at) {
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
            });
        ON_CALL(*this, OnTurnFinished(_, _, _))
            .WillByDefault([this](const TurnTelemetryContext& context, std::string_view outcome,
                                  TurnTelemetryContext::Clock::time_point finished_at) {
                static_cast<void>(context);
                static_cast<void>(finished_at);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    finished_.push_back(TurnFinishedRecord{ .outcome = std::string(outcome) });
                }
                cv_.notify_all();
            });
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
            EmittedEvent event{
                .op = "text.output",
                .turn_id = std::move(turn_id),
                .payload = std::move(text),
            };
            RecordEvent(event);
            InvokeHook(event);
        }
        on_complete(std::move(status));
    }

    void AsyncEmitAudioOutput(std::string turn_id, std::string mime_type, std::string audio_base64,
                              GatewayEmitCallback on_complete) override {
        EmittedEvent event{
            .op = "audio.output",
            .turn_id = std::move(turn_id),
            .payload = mime_type + ":" + audio_base64,
        };
        RecordEvent(event);
        InvokeHook(event);
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
            EmittedEvent event{
                .op = "error",
                .turn_id = turn_id.value_or(""),
                .payload = code + ":" + message,
            };
            RecordEvent(event);
            InvokeHook(event);
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

class RecordingGatewayMemoryStore final
    : public NiceMock<isla::server::memory::test::MockMemoryStore> {
  public:
    RecordingGatewayMemoryStore() {
        ON_CALL(*this, WarmUp()).WillByDefault(Return(absl::OkStatus()));
        ON_CALL(*this, UpsertSession(_))
            .WillByDefault([this](const isla::server::memory::MemorySessionRecord& record) {
                ++upsert_session_attempts;
                if (next_upsert_session_status_index < upsert_session_statuses.size()) {
                    const absl::Status status =
                        upsert_session_statuses[next_upsert_session_status_index++];
                    if (!status.ok()) {
                        return status;
                    }
                }
                session_records.push_back(record);
                return absl::OkStatus();
            });
        ON_CALL(*this, AppendConversationMessage(_))
            .WillByDefault([this](const isla::server::memory::ConversationMessageWrite& write) {
                message_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, ReplaceConversationItemWithEpisodeStub(_))
            .WillByDefault([](const isla::server::memory::EpisodeStubWrite& write) {
                static_cast<void>(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, SplitConversationItemWithEpisodeStub(_))
            .WillByDefault([](const isla::server::memory::SplitEpisodeStubWrite& write) {
                static_cast<void>(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, UpsertMidTermEpisode(_))
            .WillByDefault([](const isla::server::memory::MidTermEpisodeWrite& write) {
                static_cast<void>(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, ListMidTermEpisodes(_))
            .WillByDefault([](std::string_view session_id)
                               -> absl::StatusOr<std::vector<isla::server::memory::Episode>> {
                static_cast<void>(session_id);
                return std::vector<isla::server::memory::Episode>{};
            });
        ON_CALL(*this, GetMidTermEpisode(_, _))
            .WillByDefault([](std::string_view session_id, std::string_view episode_id)
                               -> absl::StatusOr<std::optional<isla::server::memory::Episode>> {
                static_cast<void>(session_id);
                static_cast<void>(episode_id);
                return std::nullopt;
            });
        ON_CALL(*this, LoadSnapshot(_))
            .WillByDefault(
                [](std::string_view session_id)
                    -> absl::StatusOr<std::optional<isla::server::memory::MemoryStoreSnapshot>> {
                    static_cast<void>(session_id);
                    return std::nullopt;
                });
    }

    std::vector<isla::server::memory::MemorySessionRecord> session_records;
    std::vector<isla::server::memory::ConversationMessageWrite> message_writes;
    std::vector<absl::Status> upsert_session_statuses;
    std::size_t next_upsert_session_status_index = 0;
    std::size_t upsert_session_attempts = 0;
};

class GatewayStubResponderStandaloneFixture : public ::testing::Test {
  protected:
    static constexpr std::string_view kSessionId = "srv_test";

    [[nodiscard]] GatewayStubResponderConfig
    MakeEchoConfig(std::chrono::milliseconds response_delay = 0ms) const {
        return GatewayStubResponderConfig{
            .response_delay = response_delay,
            .async_emit_timeout = 2s,
            .openai_client = MakeEchoOpenAiResponsesClient(),
        };
    }

    [[nodiscard]] GatewayStubResponderConfig
    MakeStoreEchoConfig(const std::shared_ptr<RecordingGatewayMemoryStore>& store) const {
        GatewayStubResponderConfig config = MakeEchoConfig();
        config.memory_user_id = "gateway_user";
        config.memory_store = store;
        return config;
    }

    void InitializeResponder(GatewayStubResponderConfig config,
                             std::string session_id = std::string(kSessionId)) {
        responder_ = std::make_unique<GatewayStubResponder>(std::move(config));
        registry_attachment_ = std::make_unique<ResponderRegistryAttachment>(*responder_);
        session_id_ = std::move(session_id);
        session_ = std::make_shared<RecordingLiveSession>(session_id_);
        registry_attachment_->registry().RegisterSession(session_);
    }

    void StartSession() {
        responder().OnSessionStarted(SessionStartedEvent{ .session_id = session_id_ });
    }

    void AcceptTurn(std::string turn_id, std::string text) {
        responder().OnTurnAccepted(TurnAcceptedEvent{
            .session_id = session_id_,
            .turn_id = std::move(turn_id),
            .text = std::move(text),
        });
    }

    [[nodiscard]] absl::StatusOr<GatewayAcceptedTurnResult>
    RunAcceptedTurnToCompletion(std::string turn_id, std::string text) {
        return responder().RunAcceptedTurnToCompletion(TurnAcceptedEvent{
            .session_id = session_id_,
            .turn_id = std::move(turn_id),
            .text = std::move(text),
        });
    }

    [[nodiscard]] std::vector<EmittedEvent> WaitForEvents(std::size_t count) const {
        EXPECT_TRUE(session().WaitForEventCount(count));
        return session().events();
    }

    [[nodiscard]] static std::string MakeLargeTurnText() {
        return std::string(24U * 1024U, 'x');
    }

    [[nodiscard]] GatewayStubResponder& responder() const {
        return *responder_;
    }

    [[nodiscard]] RecordingLiveSession& session() const {
        return *session_;
    }

    [[nodiscard]] const std::string& session_id() const {
        return session_id_;
    }

    std::unique_ptr<GatewayStubResponder> responder_;
    std::unique_ptr<ResponderRegistryAttachment> registry_attachment_;
    std::shared_ptr<RecordingLiveSession> session_;
    std::string session_id_ = std::string(kSessionId);
};

} // namespace isla::server::ai_gateway::test_support

#endif // ISLA_SERVER_SRC_AI_GATEWAY_STUB_RESPONDER_TEST_SUPPORT_HPP_
