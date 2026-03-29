#pragma once

#include <cstddef>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/llm_client.hpp"

namespace isla::server {

struct LlmRateLimitConfig {
    // Maximum average request-start rate shared across all LlmClient calls. A
    // value of 0 disables rate-per-minute limiting.
    std::size_t max_requests_per_minute = 30;
    // Token-bucket capacity used when max_requests_per_minute is enabled.
    std::size_t burst_size = 30;
    // Maximum number of in-flight LlmClient calls. A value of 0 disables
    // concurrency limiting.
    std::size_t max_concurrent_requests = 0;

    [[nodiscard]] bool enabled() const {
        return max_requests_per_minute != 0U || max_concurrent_requests != 0U;
    }
};

[[nodiscard]] absl::Status ValidateLlmRateLimitConfig(const LlmRateLimitConfig& config);

// Decorates any provider-backed LlmClient with shared request throttling. The
// limiter is provider-agnostic because it operates on the generic LlmClient
// interface rather than provider-specific request types.
[[nodiscard]] absl::StatusOr<std::shared_ptr<const LlmClient>>
CreateRateLimitedLlmClient(std::shared_ptr<const LlmClient> inner_client,
                           LlmRateLimitConfig config);

} // namespace isla::server
