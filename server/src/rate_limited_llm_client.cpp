#include "isla/server/rate_limited_llm_client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"

namespace isla::server {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

class RateLimitedLlmClient final : public LlmClient {
  public:
    RateLimitedLlmClient(std::shared_ptr<const LlmClient> inner_client, LlmRateLimitConfig config)
        : inner_client_(std::move(inner_client)), config_(config),
          available_tokens_(config_.max_requests_per_minute != 0U
                                ? static_cast<double>(config_.burst_size)
                                : 0.0),
          last_refill_at_(Clock::now()) {}

    [[nodiscard]] absl::Status Validate() const override {
        return inner_client_->Validate();
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return inner_client_->WarmUp();
    }

    [[nodiscard]] bool SupportsToolCalling() const override {
        return inner_client_->SupportsToolCalling();
    }

    [[nodiscard]] absl::Status StreamResponse(const LlmRequest& request,
                                              const LlmEventCallback& on_event) const override {
        const Permit permit = AcquirePermit();
        return inner_client_->StreamResponse(request, on_event);
    }

    [[nodiscard]] absl::StatusOr<LlmToolCallResponse>
    RunToolCallRound(const LlmToolCallRequest& request) const override {
        const Permit permit = AcquirePermit();
        return inner_client_->RunToolCallRound(request);
    }

  private:
    using Clock = std::chrono::steady_clock;

    class Permit final {
      public:
        explicit Permit(const RateLimitedLlmClient* owner) : owner_(owner) {}

        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        Permit(Permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        Permit& operator=(Permit&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            Release();
            owner_ = std::exchange(other.owner_, nullptr);
            return *this;
        }

        ~Permit() {
            Release();
        }

      private:
        void Release() {
            if (owner_ == nullptr) {
                return;
            }
            owner_->ReleasePermit();
            owner_ = nullptr;
        }

        const RateLimitedLlmClient* owner_;
    };

    void RefillTokensLocked(Clock::time_point now) const {
        if (config_.max_requests_per_minute == 0U) {
            return;
        }
        if (now <= last_refill_at_) {
            return;
        }

        const std::chrono::duration<double> elapsed = now - last_refill_at_;
        const double refill_tokens =
            elapsed.count() * static_cast<double>(config_.max_requests_per_minute) / 60.0;
        available_tokens_ = std::min<double>(static_cast<double>(config_.burst_size),
                                             available_tokens_ + refill_tokens);
        last_refill_at_ = now;
    }

    [[nodiscard]] Clock::duration TimeUntilNextTokenLocked() const {
        if (config_.max_requests_per_minute == 0U || available_tokens_ >= 1.0) {
            return Clock::duration::zero();
        }

        const double deficit = std::max(0.0, 1.0 - available_tokens_);
        const std::chrono::duration<double> wait_seconds = std::chrono::duration<double>(
            deficit * 60.0 / static_cast<double>(config_.max_requests_per_minute));
        const Clock::duration wait_duration =
            std::chrono::duration_cast<Clock::duration>(wait_seconds);
        return wait_duration > Clock::duration::zero() ? wait_duration : Clock::duration(1);
    }

    [[nodiscard]] Permit AcquirePermit() const {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            const Clock::time_point now = Clock::now();
            RefillTokensLocked(now);

            const bool concurrency_available =
                config_.max_concurrent_requests == 0U ||
                in_flight_requests_ < config_.max_concurrent_requests;
            const bool token_available =
                config_.max_requests_per_minute == 0U || available_tokens_ >= 1.0;
            if (concurrency_available && token_available) {
                if (config_.max_requests_per_minute != 0U) {
                    available_tokens_ = std::max(0.0, available_tokens_ - 1.0);
                }
                ++in_flight_requests_;
                return Permit(this);
            }

            if (!concurrency_available && config_.max_requests_per_minute == 0U) {
                cv_.wait(lock);
                continue;
            }

            const Clock::duration wait_duration = TimeUntilNextTokenLocked();
            if (wait_duration == Clock::duration::zero()) {
                cv_.wait(lock);
            } else {
                cv_.wait_for(lock, wait_duration);
            }
        }
    }

    void ReleasePermit() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_requests_ > 0U) {
            --in_flight_requests_;
        }
        cv_.notify_all();
    }

    std::shared_ptr<const LlmClient> inner_client_;
    LlmRateLimitConfig config_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable double available_tokens_;
    mutable Clock::time_point last_refill_at_;
    mutable std::size_t in_flight_requests_ = 0U;
};

} // namespace

absl::Status ValidateLlmRateLimitConfig(const LlmRateLimitConfig& config) {
    if (config.burst_size == 0U) {
        return invalid_argument("llm rate limit burst_size must be greater than zero");
    }
    if (config.max_requests_per_minute == 0U && config.max_concurrent_requests == 0U) {
        return absl::OkStatus();
    }
    if (config.max_requests_per_minute != 0U && config.burst_size == 0U) {
        return invalid_argument("llm rate limit burst_size must be greater than zero");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<const LlmClient>>
CreateRateLimitedLlmClient(std::shared_ptr<const LlmClient> inner_client,
                           LlmRateLimitConfig config) {
    if (inner_client == nullptr) {
        return invalid_argument("rate-limited llm client requires a non-null inner client");
    }
    if (const absl::Status status = ValidateLlmRateLimitConfig(config); !status.ok()) {
        return status;
    }
    if (!config.enabled()) {
        return inner_client;
    }
    return std::shared_ptr<const LlmClient>(
        std::make_shared<RateLimitedLlmClient>(std::move(inner_client), config));
}

} // namespace isla::server
