#include "isla/server/ai_gateway_stub_responder.hpp"

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
#include "isla/server/memory/prompt_loader.hpp"
#include "openai_responses_test_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;

struct EmittedEvent {
    std::string op;
    std::string turn_id;
    std::string payload;
};

std::shared_ptr<test::FakeOpenAiResponsesClient> MakeEchoOpenAiResponsesClient(
    std::string prefix = "stub echo: ") {
    return test::MakeFakeOpenAiResponsesClient(
        absl::OkStatus(), "", "resp_test", absl::OkStatus(),
        [prefix = std::move(prefix)](const OpenAiResponsesRequest& request,
                                     const OpenAiResponsesEventCallback& on_event) {
            const std::string text = prefix + request.user_text;
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
        });
}

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

class GatewayStubResponderTest : public ::testing::Test {
  protected:
    GatewayStubResponderTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 20ms,
              .openai_client = MakeEchoOpenAiResponsesClient(),
          }),
          registry_(&responder_), session_(std::make_shared<RecordingLiveSession>("srv_test")) {
        responder_.AttachSessionRegistry(&registry_);
        registry_.RegisterSession(session_);
    }

    void SetUp() override {
        responder_.OnSessionStarted(SessionStartedEvent{ .session_id = "srv_test" });
    }

    GatewayStubResponder responder_;
    GatewaySessionRegistry registry_;
    std::shared_ptr<RecordingLiveSession> session_;
};

TEST_F(GatewayStubResponderTest, SessionStartCreatesMemoryPromptBeforeAnyTurn) {
    const absl::StatusOr<std::string> prompt = responder_.RenderSessionMemoryPrompt("srv_test");
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_NE(prompt->find("<conversation>"), std::string::npos);
    EXPECT_NE(prompt->find("- (empty)"), std::string::npos);
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

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnFlowsThroughPlannerAndExecutorBoundary) {
    std::optional<ExecutionPlan> execution_plan;

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .async_emit_timeout = 2s,
        .memory_user_id = "gateway_user",
        .openai_client = MakeEchoOpenAiResponsesClient(),
        .on_execution_plan = [&](const ExecutionPlan& plan) { execution_plan = plan; },
    });
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    EXPECT_EQ(openai_step.model, "gpt-5.2");

    const std::vector<EmittedEvent> events = session->events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].op, "text.output");
    EXPECT_EQ(events[0].payload, "stub echo: hello");
    EXPECT_EQ(events[1].op, "turn.completed");
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

    responder_.OnServerStopping(registry_);

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
    registry_.OnSessionClosed(SessionClosedEvent{
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
            [builder_started, allow_finish_future](
                const OpenAiResponsesRequest& request,
                const OpenAiResponsesEventCallback& on_event) -> absl::Status {
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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

TEST(GatewayStubResponderStandaloneTest, ReplyBuilderExceptionTerminatesTurnAndWorkerContinues) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .openai_client = test::MakeFakeOpenAiResponsesClient(
            absl::OkStatus(), "", "resp_test", absl::OkStatus(),
            [](const OpenAiResponsesRequest& request,
               const OpenAiResponsesEventCallback& on_event) -> absl::Status {
            if (request.user_text == "explode") {
                throw std::runtime_error("synthetic reply builder failure");
            }
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
            [builder_started, allow_finish_future](
                const OpenAiResponsesRequest& request,
                const OpenAiResponsesEventCallback& on_event) -> absl::Status {
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
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
    GatewaySessionRegistry registry(&responder);
    auto session_one = std::make_shared<RecordingLiveSession>("srv_one");
    auto session_two = std::make_shared<RecordingLiveSession>("srv_two");
    responder.AttachSessionRegistry(&registry);
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
