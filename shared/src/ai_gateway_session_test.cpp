#include "isla/shared/ai_gateway_session.hpp"

#include <gtest/gtest.h>

namespace isla::shared::ai_gateway {
namespace {

TEST(AiGatewaySessionTest, RequiresSessionBeforeTurn) {
    SessionState state;

    const absl::Status status = state.begin_turn("turn_1");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "session is not active");
}

TEST(AiGatewaySessionTest, EnforcesSingleInFlightTurn) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123").ok());
    ASSERT_TRUE(state.begin_turn("turn_1").ok());

    const absl::Status status = state.begin_turn("turn_2");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "only one turn may be in flight per session");
}

TEST(AiGatewaySessionTest, EnforcesTextBeforeAudio) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123").ok());
    ASSERT_TRUE(state.begin_turn("turn_1").ok());

    const absl::Status status = state.mark_audio_output("turn_1");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "audio output requires text output first");

    ASSERT_TRUE(state.mark_text_output("turn_1").ok());
    ASSERT_TRUE(state.mark_audio_output("turn_1").ok());
}

TEST(AiGatewaySessionTest, TracksCancellationLifecycle) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123").ok());
    ASSERT_TRUE(state.begin_turn("turn_1").ok());
    ASSERT_TRUE(state.request_turn_cancel("turn_1").ok());
    ASSERT_TRUE(state.snapshot().active_turn.has_value());
    EXPECT_EQ(state.snapshot().active_turn->status, TurnStatus::CancelRequested);

    ASSERT_TRUE(state.confirm_turn_cancel("turn_1").ok());
    EXPECT_FALSE(state.snapshot().active_turn.has_value());
}

TEST(AiGatewaySessionTest, RejectsEndingSessionWithActiveTurn) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123").ok());
    ASSERT_TRUE(state.begin_turn("turn_1").ok());

    const absl::Status status = state.end();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.message(), "session cannot end while a turn is in flight");
}

TEST(AiGatewaySessionTest, EndsSessionAfterTurnCompletes) {
    SessionState state;
    ASSERT_TRUE(state.start("srv_123").ok());
    ASSERT_TRUE(state.begin_turn("turn_1").ok());
    ASSERT_TRUE(state.mark_text_output("turn_1").ok());
    ASSERT_TRUE(state.complete_turn("turn_1").ok());
    ASSERT_TRUE(state.end().ok());

    EXPECT_EQ(state.snapshot().status, SessionStatus::Ended);
    EXPECT_EQ(state.snapshot().session_id, "srv_123");
    EXPECT_FALSE(state.snapshot().active_turn.has_value());
}

} // namespace
} // namespace isla::shared::ai_gateway
