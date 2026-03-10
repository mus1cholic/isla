#include "isla/server/openai_llms.hpp"

#include <exception>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

} // namespace

OpenAiLLMs::OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model)
    : step_name_(std::move(step_name)), system_prompt_(std::move(system_prompt)),
      model_(std::move(model)) {}

const std::string& OpenAiLLMs::step_name() const {
    return step_name_;
}

absl::Status OpenAiLLMs::Validate() const {
    if (step_name_.empty()) {
        return invalid_argument("openai llms must include a step_name");
    }
    if (model_.empty()) {
        return invalid_argument("openai llms must include a model");
    }
    return absl::OkStatus();
}

absl::Status OpenAiLLMs::ValidateInput(std::string_view user_text) const {
    if (user_text.empty()) {
        return invalid_argument("openai llms input must include user_text");
    }
    return absl::OkStatus();
}

absl::StatusOr<ExecutionStepResult>
OpenAiLLMs::GenerateContent(std::size_t item_index, const std::string& user_text,
                            const std::string& response_prefix,
                            const OpenAiResponseBuilder& response_builder) const {
    static_cast<void>(item_index);

    const absl::Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    const absl::Status input_status = ValidateInput(user_text);
    if (!input_status.ok()) {
        return input_status;
    }

    std::string output_text;
    try {
        output_text = BuildResponse(user_text, response_prefix, response_builder);
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='"
                   << SanitizeForLog(error.what()) << "'";
        return absl::InternalError("stub responder processing failed");
    } catch (...) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='unknown exception'";
        return absl::InternalError("stub responder processing failed");
    }

    return ExecutionStepResult(LlmCallResult{
        .step_name = step_name_,
        .model = model_,
        .output_text = std::move(output_text),
    });
}

std::string OpenAiLLMs::BuildResponse(std::string_view user_text, std::string_view response_prefix,
                                      const OpenAiResponseBuilder& response_builder) const {
    if (response_builder) {
        return response_builder(response_prefix, user_text);
    }

    static_cast<void>(system_prompt_);
    return std::string(response_prefix) + std::string(user_text);
}

} // namespace isla::server::ai_gateway
