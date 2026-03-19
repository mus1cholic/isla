#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway::test {

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    using StreamHandler = std::function<absl::Status(const OpenAiResponsesRequest&,
                                                     const OpenAiResponsesEventCallback&)>;

    explicit FakeOpenAiResponsesClient(absl::Status status = absl::OkStatus(),
                                       std::string full_text = "",
                                       std::string response_id = "resp_test",
                                       absl::Status validate_status = absl::OkStatus(),
                                       StreamHandler stream_handler = {},
                                       absl::Status warmup_status = absl::OkStatus())
        : status_(std::move(status)), full_text_(std::move(full_text)),
          response_id_(std::move(response_id)), validate_status_(std::move(validate_status)),
          stream_handler_(std::move(stream_handler)), warmup_status_(std::move(warmup_status)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return validate_status_;
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return warmup_status_;
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_request = request;
            requests.push_back(request);
        }
        if (stream_handler_) {
            return stream_handler_(request, on_event);
        }
        if (!status_.ok()) {
            return status_;
        }
        const std::size_t midpoint = full_text_.size() / 2U;
        const absl::Status first_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = full_text_.substr(0, midpoint),
        });
        if (!first_status.ok()) {
            return first_status;
        }
        const absl::Status second_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = full_text_.substr(midpoint),
        });
        if (!second_status.ok()) {
            return second_status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = response_id_,
        });
    }

    [[nodiscard]] OpenAiResponsesRequest last_request_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_request;
    }

    [[nodiscard]] std::vector<OpenAiResponsesRequest> requests_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests;
    }

  private:
    mutable std::mutex mutex_;
    mutable OpenAiResponsesRequest last_request;
    mutable std::vector<OpenAiResponsesRequest> requests;
    absl::Status status_;
    std::string full_text_;
    std::string response_id_;
    absl::Status validate_status_;
    StreamHandler stream_handler_;
    absl::Status warmup_status_;
};

[[nodiscard]] inline std::shared_ptr<FakeOpenAiResponsesClient>
MakeFakeOpenAiResponsesClient(absl::Status status = absl::OkStatus(), std::string full_text = "",
                              std::string response_id = "resp_test",
                              absl::Status validate_status = absl::OkStatus(),
                              FakeOpenAiResponsesClient::StreamHandler stream_handler = {},
                              absl::Status warmup_status = absl::OkStatus()) {
    return std::make_shared<FakeOpenAiResponsesClient>(
        std::move(status), std::move(full_text), std::move(response_id), std::move(validate_status),
        std::move(stream_handler), std::move(warmup_status));
}

[[nodiscard]] inline std::string ExtractLatestPromptLine(std::string_view prompt_text) {
    std::size_t line_end = prompt_text.size();
    while (line_end > 0U) {
        const std::size_t line_break = prompt_text.rfind('\n', line_end - 1U);
        const std::size_t line_start = line_break == std::string_view::npos ? 0U : line_break + 1U;
        const std::string_view line = prompt_text.substr(line_start, line_end - line_start);
        if (line.starts_with("- [user | ") || line.starts_with("- [assistant | ") ||
            line.starts_with("- [stub | ")) {
            const std::size_t header_end = line.find("] ");
            if (header_end != std::string_view::npos) {
                return std::string(line.substr(header_end + 2U));
            }
        }
        if (line_break == std::string_view::npos) {
            break;
        }
        line_end = line_break;
    }
    return std::string(prompt_text);
}

} // namespace isla::server::ai_gateway::test
