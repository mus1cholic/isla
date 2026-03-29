#include "isla/server/rate_limited_llm_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "isla/server/llm_client.hpp"

namespace isla::server {
namespace {

using namespace std::chrono_literals;

class BlockingTestLlmClient final : public LlmClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] bool SupportsToolCalling() const override {
        return true;
    }

    [[nodiscard]] absl::Status StreamResponse(const LlmRequest& /*request*/,
                                              const LlmEventCallback& on_event) const override {
        const std::size_t index = BeginCall();
        WaitUntilReleased(index);
        return on_event(LlmCompletedEvent{
            .response_id = "stream_resp",
        });
    }

    [[nodiscard]] absl::StatusOr<LlmToolCallResponse>
    RunToolCallRound(const LlmToolCallRequest& /*request*/) const override {
        const std::size_t index = BeginCall();
        WaitUntilReleased(index);
        return LlmToolCallResponse{
            .output_text = "ok",
            .tool_calls = {},
            .continuation_token = {},
            .response_id = "tool_resp",
        };
    }

    void WaitForEnteredCount(std::size_t expected_count) const {
        std::unique_lock<std::mutex> lock(mutex_);
        ASSERT_TRUE(cv_.wait_for(lock, 2s, [&] { return call_starts_.size() >= expected_count; }));
    }

    void ReleaseOne() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ++release_count_;
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t entered_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return call_starts_.size();
    }

    [[nodiscard]] std::vector<std::chrono::steady_clock::time_point> call_starts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return call_starts_;
    }

  private:
    [[nodiscard]] std::size_t BeginCall() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t index = call_starts_.size();
        call_starts_.push_back(std::chrono::steady_clock::now());
        cv_.notify_all();
        return index;
    }

    void WaitUntilReleased(std::size_t index) const {
        std::unique_lock<std::mutex> lock(mutex_);
        ASSERT_TRUE(cv_.wait_for(lock, 2s, [&] { return release_count_ > index; }));
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable std::vector<std::chrono::steady_clock::time_point> call_starts_;
    mutable std::size_t release_count_ = 0U;
};

TEST(RateLimitedLlmClientTest, FactoryRejectsNullInnerClient) {
    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateRateLimitedLlmClient(nullptr, LlmRateLimitConfig{
                                                .max_concurrent_requests = 1,
                                            });

    ASSERT_FALSE(client.ok());
    EXPECT_EQ(client.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RateLimitedLlmClientTest, FactoryReturnsInnerClientWhenLimitsDisabled) {
    const auto inner = std::make_shared<const BlockingTestLlmClient>();

    const absl::StatusOr<std::shared_ptr<const LlmClient>> client =
        CreateRateLimitedLlmClient(inner, LlmRateLimitConfig{
                                              .max_requests_per_minute = 0,
                                              .burst_size = 1,
                                              .max_concurrent_requests = 0,
                                          });

    ASSERT_TRUE(client.ok()) << client.status();
    EXPECT_EQ(client->get(), inner.get());
}

TEST(RateLimitedLlmClientTest, ConcurrencyLimitIsSharedAcrossRequestTypes) {
    const auto inner = std::make_shared<BlockingTestLlmClient>();
    const absl::StatusOr<std::shared_ptr<const LlmClient>> limited =
        CreateRateLimitedLlmClient(inner, LlmRateLimitConfig{
                                              .max_concurrent_requests = 1,
                                          });
    ASSERT_TRUE(limited.ok()) << limited.status();

    std::atomic<bool> second_finished = false;
    std::thread first([&] {
        const absl::Status status = (*limited)->StreamResponse(
            LlmRequest{
                .model = "test-model",
                .user_text = "hello",
            },
            [](const LlmEvent&) { return absl::OkStatus(); });
        EXPECT_TRUE(status.ok()) << status;
    });

    inner->WaitForEnteredCount(1U);

    std::thread second([&] {
        const absl::StatusOr<LlmToolCallResponse> response =
            (*limited)->RunToolCallRound(LlmToolCallRequest{
                .model = "test-model",
                .user_text = "hello",
            });
        EXPECT_TRUE(response.ok()) << response.status();
        second_finished.store(true);
    });

    std::this_thread::sleep_for(40ms);
    EXPECT_EQ(inner->entered_count(), 1U);
    EXPECT_FALSE(second_finished.load());

    inner->ReleaseOne();
    inner->WaitForEnteredCount(2U);
    inner->ReleaseOne();

    first.join();
    second.join();
}

TEST(RateLimitedLlmClientTest, RequestsPerMinuteLimitDelaysSubsequentStarts) {
    const auto inner = std::make_shared<BlockingTestLlmClient>();
    const absl::StatusOr<std::shared_ptr<const LlmClient>> limited =
        CreateRateLimitedLlmClient(inner, LlmRateLimitConfig{
                                              .max_requests_per_minute = 3000,
                                              .burst_size = 1,
                                          });
    ASSERT_TRUE(limited.ok()) << limited.status();

    std::thread first([&] {
        const absl::Status status = (*limited)->StreamResponse(
            LlmRequest{
                .model = "test-model",
                .user_text = "first",
            },
            [](const LlmEvent&) { return absl::OkStatus(); });
        EXPECT_TRUE(status.ok()) << status;
    });
    inner->WaitForEnteredCount(1U);

    std::thread second([&] {
        const absl::Status status = (*limited)->StreamResponse(
            LlmRequest{
                .model = "test-model",
                .user_text = "second",
            },
            [](const LlmEvent&) { return absl::OkStatus(); });
        EXPECT_TRUE(status.ok()) << status;
    });

    inner->WaitForEnteredCount(2U);
    inner->ReleaseOne();
    inner->ReleaseOne();
    first.join();
    second.join();

    const std::vector<std::chrono::steady_clock::time_point> starts = inner->call_starts();
    ASSERT_EQ(starts.size(), 2U);
    const auto delta_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(starts[1] - starts[0]);
    EXPECT_GE(delta_ms.count(), 15);
}

} // namespace
} // namespace isla::server
