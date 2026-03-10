#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_planner.hpp"

namespace isla::server::ai_gateway {

class OpenAiLLMs final {
  public:
    OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model);

    [[nodiscard]] const std::string& step_name() const;
    [[nodiscard]] absl::Status Validate() const;
    [[nodiscard]] absl::StatusOr<ExecutionStepResult>
    GenerateContent(std::size_t item_index, const std::string& user_text,
                    const std::string& response_prefix,
                    const OpenAiResponseBuilder& response_builder) const;

  private:
    [[nodiscard]] absl::Status ValidateInput(std::string_view user_text) const;
    [[nodiscard]] std::string BuildResponse(std::string_view user_text,
                                            std::string_view response_prefix,
                                            const OpenAiResponseBuilder& response_builder) const;

    std::string step_name_;
    std::string system_prompt_;
    std::string model_;
};

} // namespace isla::server::ai_gateway
