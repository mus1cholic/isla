#include "isla/server/openai_responses_sse_parser.hpp"

#include <algorithm>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "ai_gateway_string_utils.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_responses_json_utils.hpp"
#include <nlohmann/json.hpp>

namespace isla::server::ai_gateway {
namespace {

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

absl::StatusOr<std::vector<OpenAiResponsesOutputItem>>
ParseResponseOutputItems(const nlohmann::json& event_json) {
    std::vector<OpenAiResponsesOutputItem> output_items;
    if (!event_json.contains("response") || !event_json["response"].is_object()) {
        return output_items;
    }
    const nlohmann::json& response = event_json["response"];
    if (!response.contains("output") || !response["output"].is_array()) {
        return output_items;
    }
    output_items.reserve(response["output"].size());
    for (const auto& item : response["output"]) {
        if (!item.is_object()) {
            continue;
        }
        const absl::StatusOr<std::optional<std::string>> type =
            ReadOptionalStringField(item, "type");
        if (!type.ok()) {
            return type.status();
        }
        OpenAiResponsesOutputItem output_item{
            .type = type->value_or(""),
            .raw_json = item.dump(),
            .call_id = std::nullopt,
            .name = std::nullopt,
            .arguments_json = std::nullopt,
        };
        if (output_item.type == "function_call") {
            const absl::StatusOr<std::optional<std::string>> call_id =
                ReadOptionalStringField(item, "call_id");
            if (!call_id.ok()) {
                return call_id.status();
            }
            const absl::StatusOr<std::optional<std::string>> name =
                ReadOptionalStringField(item, "name");
            if (!name.ok()) {
                return name.status();
            }
            const absl::StatusOr<std::optional<std::string>> arguments =
                ReadOptionalStringField(item, "arguments");
            if (!arguments.ok()) {
                return arguments.status();
            }
            output_item.call_id = *call_id;
            output_item.name = *name;
            output_item.arguments_json = *arguments;
        }
        output_items.push_back(std::move(output_item));
    }
    return output_items;
}

std::optional<std::string> ExtractCompletedText(const nlohmann::json& event_json) {
    if (!event_json.contains("response") || !event_json["response"].is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& response = event_json["response"];
    if (!response.contains("output") || !response["output"].is_array()) {
        return std::nullopt;
    }

    std::string text;
    for (const auto& item : response["output"]) {
        if (!item.is_object()) {
            continue;
        }
        if (!item.contains("content") || !item["content"].is_array()) {
            continue;
        }
        for (const auto& part : item["content"]) {
            if (!part.is_object()) {
                continue;
            }
            const absl::StatusOr<std::optional<std::string>> part_type =
                ReadOptionalStringField(part, "type");
            if (!part_type.ok() || part_type->value_or("") != "output_text") {
                continue;
            }
            if (!part.contains("text") || !part["text"].is_string()) {
                continue;
            }
            text += part["text"].get<std::string>();
        }
    }

    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

absl::Status MapProviderEventError(const nlohmann::json& event_json) {
    const absl::StatusOr<std::optional<std::string>> type_field =
        ReadOptionalStringField(event_json, "type");
    if (!type_field.ok()) {
        return type_field.status();
    }
    const std::string type = type_field->value_or("");
    if (type == "error") {
        std::string message = ExtractJsonErrorMessage(event_json);
        if (message.empty()) {
            message = "openai responses stream returned an error event";
        }
        return absl::UnavailableError(message);
    }
    if (type == "response.failed") {
        std::string message = "openai responses request failed";
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const auto& response = event_json["response"];
            if (response.contains("error") && response["error"].is_object()) {
                const std::string extracted = ExtractJsonErrorMessage(response);
                if (!extracted.empty()) {
                    message = extracted;
                }
            }
        }
        return absl::UnavailableError(message);
    }
    if (type == "response.incomplete") {
        std::string message = "openai responses request completed incompletely";
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const auto& response = event_json["response"];
            if (response.contains("incomplete_details") &&
                response["incomplete_details"].is_object()) {
                const auto& details = response["incomplete_details"];
                const absl::StatusOr<std::optional<std::string>> reason =
                    ReadOptionalStringField(details, "reason");
                if (!reason.ok()) {
                    return reason.status();
                }
                if (reason->has_value()) {
                    message = "openai responses incomplete: " + **reason;
                }
            }
        }
        return absl::UnavailableError(message);
    }
    return absl::OkStatus();
}

absl::Status DispatchStreamEvent(const nlohmann::json& event_json, SseParseSummary* summary,
                                 const OpenAiResponsesEventCallback& on_event) {
    const absl::StatusOr<std::optional<std::string>> type_field =
        ReadOptionalStringField(event_json, "type");
    if (!type_field.ok()) {
        return type_field.status();
    }
    const std::string type = type_field->value_or("");
    if (type.empty()) {
        return absl::OkStatus();
    }
    ++summary->event_count;

    absl::Status provider_error = MapProviderEventError(event_json);
    if (!provider_error.ok()) {
        return provider_error;
    }

    if (type == "response.output_text.delta") {
        const absl::StatusOr<std::optional<std::string>> delta =
            ReadOptionalStringField(event_json, "delta");
        if (!delta.ok()) {
            return delta.status();
        }
        summary->saw_delta = true;
        return on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = delta->value_or(""),
        });
    }
    if (type == "response.completed") {
        const absl::StatusOr<std::vector<OpenAiResponsesOutputItem>> output_items =
            ParseResponseOutputItems(event_json);
        if (!output_items.ok()) {
            return output_items.status();
        }
        if (!summary->saw_delta) {
            const std::optional<std::string> completed_text = ExtractCompletedText(event_json);
            const bool has_function_call =
                std::find_if(output_items->begin(), output_items->end(),
                             [](const OpenAiResponsesOutputItem& output_item) {
                                 return output_item.type == "function_call";
                             }) != output_items->end();
            if ((!completed_text.has_value() || completed_text->empty()) && !has_function_call) {
                return internal_error(
                    "openai responses completed without any recoverable output text");
            }
            if (completed_text.has_value() && !completed_text->empty()) {
                const absl::Status delta_status =
                    on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = *completed_text });
                if (!delta_status.ok()) {
                    return delta_status;
                }
            }
        }
        summary->saw_completed = true;
        std::string response_id;
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const absl::StatusOr<std::optional<std::string>> id =
                ReadOptionalStringField(event_json["response"], "id");
            if (!id.ok()) {
                return id.status();
            }
            response_id = id->value_or("");
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = std::move(response_id),
            .output_items = *output_items,
        });
    }

    return absl::OkStatus();
}

} // namespace

absl::StatusOr<SseFeedDisposition>
IncrementalSseParser::Feed(std::string_view chunk, const OpenAiResponsesEventCallback& on_event) {
    line_buffer_.append(chunk.data(), chunk.size());
    for (;;) {
        const std::size_t newline = line_buffer_.find('\n');
        if (newline == std::string::npos) {
            return SseFeedDisposition::kContinue;
        }

        std::string line = line_buffer_.substr(0, newline);
        line_buffer_.erase(0, newline + 1U);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const absl::StatusOr<SseFeedDisposition> disposition = ProcessLine(line, on_event);
        if (!disposition.ok()) {
            return disposition.status();
        }
        if (*disposition == SseFeedDisposition::kCompleted) {
            line_buffer_.clear();
            return disposition;
        }
    }
}

absl::StatusOr<SseParseSummary>
IncrementalSseParser::Finish(const OpenAiResponsesEventCallback& on_event) {
    if (!line_buffer_.empty()) {
        std::string trailing_line = std::move(line_buffer_);
        line_buffer_.clear();
        if (!trailing_line.empty() && trailing_line.back() == '\r') {
            trailing_line.pop_back();
        }
        const absl::StatusOr<SseFeedDisposition> disposition = ProcessLine(trailing_line, on_event);
        if (!disposition.ok()) {
            return disposition.status();
        }
    }

    const absl::StatusOr<SseFeedDisposition> trailing_flush = FlushBufferedEvent(on_event);
    if (!trailing_flush.ok()) {
        return trailing_flush.status();
    }
    if (!summary_.saw_completed) {
        return internal_error("openai responses stream ended before completion");
    }
    return summary_;
}

absl::StatusOr<SseFeedDisposition>
IncrementalSseParser::ProcessLine(const std::string& line,
                                  const OpenAiResponsesEventCallback& on_event) {
    if (line.empty()) {
        return FlushBufferedEvent(on_event);
    }
    if (line.starts_with("data:")) {
        std::string_view payload = std::string_view(line).substr(5);
        // NOTICE: OpenAI Responses may emit one JSON event across multiple SSE `data:` lines.
        // Re-inserting literal newlines between those fragments is valid generic SSE behavior,
        // but it corrupts provider JSON when the split lands inside a quoted string (for example
        // a long `instructions` field echoed in `response.created`). Preserve only the single
        // optional leading space after `data:` and concatenate the fragments directly.
        if (!payload.empty() && payload.front() == ' ') {
            payload.remove_prefix(1U);
        }
        data_.append(payload);
    } else if (line.starts_with("event:")) {
        event_name_ = TrimAscii(std::string_view(line).substr(6));
    } else if (line.starts_with(":")) {
        // SSE comment — ignore.
    } else if (!data_.empty()) {
        // Continuation line without a `data:` prefix. This can happen when the provider
        // leaks a literal newline into a `data:` field value (e.g. inside a long JSON
        // string such as `instructions` echoed in `response.created`). The Feed loop
        // splits on '\n', so the remainder lands here as a bare line. Insert a space to
        // replace the consumed newline (harmless between JSON tokens, and prevents word-
        // merging inside JSON strings) then append the continuation.
        data_ += ' ';
        data_.append(line);
    }
    return SseFeedDisposition::kContinue;
}

absl::StatusOr<SseFeedDisposition>
IncrementalSseParser::FlushBufferedEvent(const OpenAiResponsesEventCallback& on_event) {
    static_cast<void>(event_name_);
    if (data_.empty()) {
        event_name_.clear();
        return SseFeedDisposition::kContinue;
    }
    if (data_ == "[DONE]") {
        event_name_.clear();
        data_.clear();
        return SseFeedDisposition::kContinue;
    }

    nlohmann::json event_json;
    try {
        event_json = nlohmann::json::parse(data_);
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway openai responses stream parse failed detail='"
                   << SanitizeForLog(error.what()) << "'";
        return internal_error("openai responses stream contained invalid JSON");
    }

    const absl::StatusOr<std::optional<std::string>> type_field =
        ReadOptionalStringField(event_json, "type");
    const std::string event_type =
        type_field.ok() ? type_field->value_or("") : std::string("<unreadable>");
    VLOG(3) << "AI gateway openai responses assembled SSE event event_index="
            << (summary_.event_count + 1U) << " type='" << SanitizeForLog(event_type)
            << "' event_name='" << SanitizeForLog(event_name_) << "' payload_bytes=" << data_.size()
            << " first_event=" << (summary_.event_count == 0U ? "true" : "false");

    event_name_.clear();
    data_.clear();
    absl::Status dispatch_status = DispatchStreamEvent(event_json, &summary_, on_event);
    if (!dispatch_status.ok()) {
        return dispatch_status;
    }
    if (summary_.saw_completed) {
        return SseFeedDisposition::kCompleted;
    }
    return SseFeedDisposition::kContinue;
}

} // namespace isla::server::ai_gateway
