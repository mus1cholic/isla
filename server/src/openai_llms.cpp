#include "isla/server/openai_llms.hpp"

#include <exception>
#include <memory>
#include <string_view>
#include <type_traits>
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

OpenAiLLMs::OpenAiLLMs(std::string step_name, std::string system_prompt, std::string model,
                       std::shared_ptr<const OpenAiResponsesClient> responses_client,
                       std::string response_prefix, OpenAiResponseBuilder response_builder)
    : step_name_(std::move(step_name)), system_prompt_(std::move(system_prompt)),
      model_(std::move(model)), responses_client_(std::move(responses_client)),
      response_prefix_(std::move(response_prefix)), response_builder_(std::move(response_builder)) {
}

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
OpenAiLLMs::GenerateContent(std::size_t item_index, const std::string& user_text) const {
    static_cast<void>(item_index);

    absl::Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    absl::Status input_status = ValidateInput(user_text);
    if (!input_status.ok()) {
        return input_status;
    }

    std::string output_text;
    try {
        if (responses_client_ != nullptr) {
            const absl::StatusOr<std::string> provider_text =
                GenerateProviderResponse(item_index, user_text);
            if (!provider_text.ok()) {
                return provider_text.status();
            }
            output_text = *provider_text;
        } else {
            // TODO(ai-gateway): Remove this fallback path in Phase 3.6.
            output_text = BuildResponse(user_text, response_prefix_, response_builder_);
        }
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='" << SanitizeForLog(error.what())
                   << "'";
        return absl::InternalError("openai llms response building failed");
    } catch (...) {
        LOG(ERROR) << "AI gateway openai llms response building failed step_name='"
                   << SanitizeForLog(step_name_) << "' detail='unknown exception'";
        return absl::InternalError("openai llms response building failed");
    }

    return ExecutionStepResult(LlmCallResult{
        .step_name = step_name_,
        .model = model_,
        .output_text = std::move(output_text),
    });
}

absl::StatusOr<std::string> OpenAiLLMs::GenerateProviderResponse(std::size_t item_index,
                                                                 std::string_view user_text) const {
    static_cast<void>(item_index);

    absl::Status client_status = responses_client_->Validate();
    if (!client_status.ok()) {
        return client_status;
    }

    VLOG(1) << "AI gateway openai llms dispatching provider request step_name='"
            << SanitizeForLog(step_name_) << "' model='" << SanitizeForLog(model_)
            << "' user_text_bytes=" << user_text.size()
            << " system_prompt_present=" << (!system_prompt_.empty() ? "true" : "false");

    std::string output_text;
    absl::Status stream_status = responses_client_->StreamResponse(
        OpenAiResponsesRequest{
            .model = model_,
            .system_prompt = system_prompt_,
            .user_text = std::string(user_text),
        },
        [&output_text](const OpenAiResponsesEvent& event) -> absl::Status {
            return std::visit(
                [&output_text](const auto& concrete_event) -> absl::Status {
                    using Event = std::decay_t<decltype(concrete_event)>;
                    if constexpr (std::is_same_v<Event, OpenAiResponsesTextDeltaEvent>) {
                        output_text.append(concrete_event.text_delta);
                    }
                    return absl::OkStatus();
                },
                event);
        });
    if (!stream_status.ok()) {
        LOG(ERROR) << "AI gateway openai llms provider request failed step_name='"
                   << SanitizeForLog(step_name_) << "' model='" << SanitizeForLog(model_)
                   << "' detail='" << SanitizeForLog(stream_status.message()) << "'";
        return stream_status;
    }
    return output_text;
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
