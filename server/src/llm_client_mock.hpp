#pragma once

#include <gmock/gmock.h>

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "isla/server/llm_client.hpp"

namespace isla::server::test {

inline absl::Status EmitLlmResponse(std::string_view text, const LlmEventCallback& on_event,
                                    std::string_view response_id = "resp_test") {
    if (!text.empty()) {
        const absl::Status delta_status = on_event(LlmTextDeltaEvent{
            .text_delta = std::string(text),
        });
        if (!delta_status.ok()) {
            return delta_status;
        }
    }
    return on_event(LlmCompletedEvent{
        .response_id = std::string(response_id),
    });
}

class MockLlmClient : public LlmClient {
  public:
    MOCK_METHOD(absl::Status, Validate, (), (const, override));
    MOCK_METHOD(absl::Status, WarmUp, (), (const, override));
    MOCK_METHOD(absl::Status, StreamResponse,
                (const LlmRequest& request, const LlmEventCallback& on_event), (const, override));
};

} // namespace isla::server::test
