#pragma once

#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway::test {

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit FakeOpenAiResponsesClient(absl::Status status = absl::OkStatus(),
                                       std::string full_text = "",
                                       std::string response_id = "resp_test")
        : status_(std::move(status)),
          full_text_(std::move(full_text)),
          response_id_(std::move(response_id)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        last_request = request;
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
};

[[nodiscard]] inline std::shared_ptr<FakeOpenAiResponsesClient> MakeFakeOpenAiResponsesClient(
    absl::Status status = absl::OkStatus(), std::string full_text = "",
    std::string response_id = "resp_test") {
    return std::make_shared<FakeOpenAiResponsesClient>(std::move(status), std::move(full_text),
                                                       std::move(response_id));
}

} // namespace isla::server::ai_gateway::test
