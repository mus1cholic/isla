#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_planner.hpp"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

class OpenAiLLMs final {
  public:
    OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model,
               std::shared_ptr<const OpenAiResponsesClient> responses_client = nullptr);

    [[nodiscard]] const std::string& step_name() const;
    [[nodiscard]] absl::Status Validate() const;
    [[nodiscard]] absl::StatusOr<ExecutionStepResult> GenerateContent(std::size_t item_index,
                                                                      const std::string& user_text)
        const;

  private:
    [[nodiscard]] absl::Status ValidateInput(std::string_view user_text) const;
    [[nodiscard]] absl::StatusOr<std::string>
    GenerateProviderResponse(std::size_t item_index, std::string_view user_text) const;

    std::string step_name_;
    std::string system_prompt_;
    std::string model_;
    std::shared_ptr<const OpenAiResponsesClient> responses_client_;
};

} // namespace isla::server::ai_gateway
