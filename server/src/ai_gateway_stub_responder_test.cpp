#include "ai_gateway_stub_responder.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
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
        RecordEvent({
            .op = "turn.completed",
            .turn_id = std::move(turn_id),
            .payload = "",
        });
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
        on_complete(absl::OkStatus());
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

  private:
    void RecordEvent(EmittedEvent event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        cv_.notify_all();
    }

    std::string session_id_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<EmittedEvent> events_;
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
          registry_(&responder_),
          session_(std::make_shared<RecordingLiveSession>("srv_test")) {
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

} // namespace
} // namespace isla::server::ai_gateway
