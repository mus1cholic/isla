#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

struct SseParseSummary {
    bool saw_delta = false;
    bool saw_completed = false;
    std::size_t event_count = 0;
};

enum class SseFeedDisposition {
    kContinue,
    kCompleted,
};

class IncrementalSseParser final {
  public:
    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    Feed(std::string_view chunk, const OpenAiResponsesEventCallback& on_event);

    [[nodiscard]] absl::StatusOr<SseParseSummary>
    Finish(const OpenAiResponsesEventCallback& on_event);

  private:
    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    ProcessLine(const std::string& line, const OpenAiResponsesEventCallback& on_event);

    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    FlushBufferedEvent(const OpenAiResponsesEventCallback& on_event);

    std::string line_buffer_;
    std::string event_name_;
    std::string data_;
    SseParseSummary summary_;
};

} // namespace isla::server::ai_gateway
