#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

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
                                       StreamHandler stream_handler = {})
        : status_(std::move(status)),
          full_text_(std::move(full_text)),
          response_id_(std::move(response_id)),
          validate_status_(std::move(validate_status)),
          stream_handler_(std::move(stream_handler)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return validate_status_;
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        last_request = request;
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

    mutable OpenAiResponsesRequest last_request;

  private:
    absl::Status status_;
    std::string full_text_;
    std::string response_id_;
    absl::Status validate_status_;
    StreamHandler stream_handler_;
};

[[nodiscard]] inline std::shared_ptr<FakeOpenAiResponsesClient> MakeFakeOpenAiResponsesClient(
    absl::Status status = absl::OkStatus(), std::string full_text = "",
    std::string response_id = "resp_test", absl::Status validate_status = absl::OkStatus(),
    FakeOpenAiResponsesClient::StreamHandler stream_handler = {}) {
    return std::make_shared<FakeOpenAiResponsesClient>(
        std::move(status), std::move(full_text), std::move(response_id),
        std::move(validate_status), std::move(stream_handler));
}

} // namespace isla::server::ai_gateway::test
