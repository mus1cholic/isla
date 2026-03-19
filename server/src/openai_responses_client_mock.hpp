#pragma once

#include <gmock/gmock.h>

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway::test {

inline absl::Status EmitOpenAiResponse(std::string_view text,
                                       const OpenAiResponsesEventCallback& on_event,
                                       std::string_view response_id = "resp_test") {
    if (!text.empty()) {
        const absl::Status delta_status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = std::string(text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
    }
    return on_event(OpenAiResponsesCompletedEvent{
        .response_id = std::string(response_id),
    });
}

class MockOpenAiResponsesClient : public OpenAiResponsesClient {
  public:
    MOCK_METHOD(absl::Status, Validate, (), (const, override));
    MOCK_METHOD(absl::Status, WarmUp, (), (const, override));
    MOCK_METHOD(absl::Status, StreamResponse,
                (const OpenAiResponsesRequest& request,
                 const OpenAiResponsesEventCallback& on_event),
                (const, override));
};

} // namespace isla::server::ai_gateway::test
