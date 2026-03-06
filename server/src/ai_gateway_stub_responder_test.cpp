#include "ai_gateway_stub_responder.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "ai_gateway_server.hpp"

namespace isla::server::ai_gateway {
namespace {

using namespace std::chrono_literals;

struct EmittedEvent {
    std::string op;
    std::string turn_id;
    std::string payload;
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
        RecordEvent({ .op = "text.output", .turn_id = std::move(turn_id), .payload = text });
        absl::Status status = absl::OkStatus();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fail_next_text_output_) {
                fail_next_text_output_ = false;
                status = absl::UnavailableError("text output failed");
            }
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
        RecordEvent({
            .op = "error",
            .turn_id = turn_id.value_or(""),
            .payload = code + ":" + message,
        });
        bool delay_completion = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            delay_completion = delay_next_error_completion_;
            if (delay_completion) {
                delay_next_error_completion_ = false;
                pending_error_completion_ = std::move(on_complete);
            }
        }
        if (!delay_completion) {
            on_complete(absl::OkStatus());
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
    bool closed_ = false;
};

class GatewayStubResponderTest : public ::testing::Test {
  protected:
    GatewayStubResponderTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 20ms,
              .response_prefix = "stub echo: ",
          }),
          registry_(&responder_), session_(std::make_shared<RecordingLiveSession>("srv_test")) {
        responder_.AttachSessionRegistry(&registry_);
        registry_.RegisterSession(session_);
    }

    GatewayStubResponder responder_;
    GatewaySessionRegistry registry_;
    std::shared_ptr<RecordingLiveSession> session_;
};

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

TEST_F(GatewayStubResponderTest, EmitFailureDoesNotBlockLaterTurns) {
    session_->FailNextTextOutput();

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_1",
        .text = "first",
    });

    ASSERT_TRUE(session_->WaitForEventCount(1U));
    {
        const std::vector<EmittedEvent> first_events = session_->events();
        ASSERT_EQ(first_events.size(), 1U);
        EXPECT_EQ(first_events[0].op, "text.output");
        EXPECT_EQ(first_events[0].turn_id, "turn_1");
    }

    responder_.OnTurnAccepted(TurnAcceptedEvent{
        .session_id = "srv_test",
        .turn_id = "turn_2",
        .text = "second",
    });

    ASSERT_TRUE(session_->WaitForEventCount(3U));
    const std::vector<EmittedEvent> events = session_->events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1].op, "text.output");
    EXPECT_EQ(events[1].turn_id, "turn_2");
    EXPECT_EQ(events[1].payload, "stub echo: second");
    EXPECT_EQ(events[2].op, "turn.completed");
    EXPECT_EQ(events[2].turn_id, "turn_2");
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

TEST(GatewayStubResponderStandaloneTest, ReplyBuilderExceptionTerminatesTurnAndWorkerContinues) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .response_prefix = "stub echo: ",
        .reply_builder = [](std::string_view prefix, std::string_view text) -> std::string {
            static_cast<void>(prefix);
            if (text == "explode") {
                throw std::runtime_error("synthetic reply builder failure");
            }
            return std::string("stub echo: ") + std::string(text);
        },
    });
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
    registry.RegisterSession(session);

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
        EXPECT_EQ(events[0].payload, "internal_error:stub responder processing failed");
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

TEST(GatewayStubResponderStandaloneTest, MatchingCancelForInProgressTurnEmitsCancelled) {
    auto builder_started = std::make_shared<std::promise<void>>();
    std::future<void> started_future = builder_started->get_future();
    auto allow_finish = std::make_shared<std::promise<void>>();
    std::shared_future<void> allow_finish_future = allow_finish->get_future().share();

    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .response_prefix = "stub echo: ",
        .reply_builder = [builder_started, allow_finish_future](
                             std::string_view prefix, std::string_view text) -> std::string {
            builder_started->set_value();
            allow_finish_future.wait();
            return std::string(prefix) + std::string(text);
        },
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

TEST(GatewayStubResponderStandaloneTest, AcceptedTurnDuringShutdownDoesNotBlockOnDelayedEmit) {
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 0ms,
        .response_prefix = "stub echo: ",
    });
    GatewaySessionRegistry registry(&responder);
    auto session = std::make_shared<RecordingLiveSession>("srv_test");
    responder.AttachSessionRegistry(&registry);
    registry.RegisterSession(session);

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

} // namespace
} // namespace isla::server::ai_gateway
