#include "isla/server/openai_responses_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

struct SseParseSummary {
    bool saw_delta = false;
    bool saw_completed = false;
    std::size_t event_count = 0;
};

std::atomic<std::uint64_t> g_temp_file_counter{ 0 };

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

std::string TrimAscii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string BuildCurlUrl(const OpenAiResponsesClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port) + config.target;
}

#if defined(_WIN32)
constexpr std::string_view kCurlCommand = "curl.exe";

std::string ShellQuote(std::string_view value) {
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

using PipeHandle = FILE*;

PipeHandle OpenPipe(const std::string& command) {
    return _popen(command.c_str(), "rb");
}

int ClosePipe(PipeHandle pipe) {
    return _pclose(pipe);
}
#else
constexpr std::string_view kCurlCommand = "curl";

std::string ShellQuote(std::string_view value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

using PipeHandle = FILE*;

PipeHandle OpenPipe(const std::string& command) {
    return popen(command.c_str(), "r");
}

int ClosePipe(PipeHandle pipe) {
    return pclose(pipe);
}
#endif

std::filesystem::path MakeTempPath(std::string_view suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t sequence = g_temp_file_counter.fetch_add(1U);
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    return directory / ("isla_openai_" + std::to_string(now) + "_" + std::to_string(sequence) +
                        std::string(suffix));
}

class ScopedTempFile final {
  public:
    explicit ScopedTempFile(std::string_view suffix) : path_(MakeTempPath(suffix)) {}

    ~ScopedTempFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string ReadFileToString(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<unsigned int> ParseHttpStatusCode(std::string_view header_text) {
    std::optional<unsigned int> status_code;
    std::size_t cursor = 0;
    while (cursor <= header_text.size()) {
        const std::size_t next_newline = header_text.find('\n', cursor);
        const std::size_t line_end =
            next_newline == std::string_view::npos ? header_text.size() : next_newline;
        std::string line(header_text.substr(cursor, line_end - cursor));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("HTTP/", 0) == 0) {
            const std::size_t first_space = line.find(' ');
            const std::size_t second_space = first_space == std::string::npos
                                                 ? std::string::npos
                                                 : line.find(' ', first_space + 1U);
            if (first_space != std::string::npos) {
                const std::string_view code_text = std::string_view(line).substr(
                    first_space + 1U,
                    (second_space == std::string::npos ? line.size() : second_space) -
                        (first_space + 1U));
                unsigned int parsed_status_code = 0;
                const auto parse_result = std::from_chars(
                    code_text.data(), code_text.data() + code_text.size(), parsed_status_code);
                if (parse_result.ec == std::errc() &&
                    parse_result.ptr == code_text.data() + code_text.size()) {
                    status_code = parsed_status_code;
                }
            }
        }

        if (next_newline == std::string_view::npos) {
            break;
        }
        cursor = next_newline + 1U;
    }
    return status_code;
}

absl::Status MapHttpErrorStatus(unsigned int status_code, std::string message) {
    switch (status_code) {
    case 400:
    case 404:
    case 409:
    case 422:
        return absl::InvalidArgumentError(std::move(message));
    case 401:
        return absl::UnauthenticatedError(std::move(message));
    case 403:
        return absl::PermissionDeniedError(std::move(message));
    case 408:
    case 504:
        return absl::DeadlineExceededError(std::move(message));
    case 429:
    case 500:
    case 502:
    case 503:
        return absl::UnavailableError(std::move(message));
    default:
        return absl::InternalError(std::move(message));
    }
}

std::string ExtractJsonErrorMessage(const nlohmann::json& json) {
    if (json.contains("error") && json["error"].is_object()) {
        const auto& error = json["error"];
        if (error.contains("message") && error["message"].is_string()) {
            return error["message"].get<std::string>();
        }
    }
    if (json.contains("message") && json["message"].is_string()) {
        return json["message"].get<std::string>();
    }
    return {};
}

std::string BuildHttpErrorMessage(unsigned int status_code, std::string_view body) {
    std::string message = "openai responses request failed";
    if (!body.empty()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(body);
            const std::string extracted = ExtractJsonErrorMessage(parsed);
            if (!extracted.empty()) {
                return extracted;
            }
        } catch (const std::exception& error) {
            VLOG(1) << "AI gateway openai responses error body was not JSON detail='"
                    << SanitizeForLog(error.what()) << "'";
        }
    }
    return message + " with status " + std::to_string(status_code);
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
            if (part.value("type", "") != "output_text") {
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
    const std::string type = event_json.value("type", "");
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
                if (details.contains("reason") && details["reason"].is_string()) {
                    message =
                        "openai responses incomplete: " + details["reason"].get<std::string>();
                }
            }
        }
        return absl::UnavailableError(message);
    }
    return absl::OkStatus();
}

absl::Status DispatchStreamEvent(const nlohmann::json& event_json, SseParseSummary* summary,
                                 const OpenAiResponsesEventCallback& on_event) {
    const std::string type = event_json.value("type", "");
    if (type.empty()) {
        return absl::OkStatus();
    }
    ++summary->event_count;

    absl::Status provider_error = MapProviderEventError(event_json);
    if (!provider_error.ok()) {
        return provider_error;
    }

    if (type == "response.output_text.delta") {
        summary->saw_delta = true;
        return on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = event_json.value("delta", ""),
        });
    }
    if (type == "response.completed") {
        if (!summary->saw_delta) {
            const std::optional<std::string> completed_text = ExtractCompletedText(event_json);
            if (!completed_text.has_value() || completed_text->empty()) {
                return internal_error(
                    "openai responses completed without any recoverable output text");
            }
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = *completed_text });
            if (!delta_status.ok()) {
                return delta_status;
            }
        }
        summary->saw_completed = true;
        std::string response_id;
        if (event_json.contains("response") && event_json["response"].is_object()) {
            response_id = event_json["response"].value("id", "");
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = std::move(response_id),
        });
    }

    return absl::OkStatus();
}

absl::StatusOr<SseParseSummary> ParseSseBody(std::string_view body,
                                             const OpenAiResponsesEventCallback& on_event) {
    SseParseSummary summary;
    std::string event_name;
    std::string data;

    const auto flush_event = [&]() -> absl::StatusOr<bool> {
        static_cast<void>(event_name);
        if (data.empty()) {
            event_name.clear();
            return false;
        }
        if (data == "[DONE]") {
            event_name.clear();
            data.clear();
            return false;
        }

        nlohmann::json event_json;
        try {
            event_json = nlohmann::json::parse(data);
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway openai responses stream parse failed detail='"
                       << SanitizeForLog(error.what()) << "'";
            return internal_error("openai responses stream contained invalid JSON");
        }
        event_name.clear();
        data.clear();
        absl::Status dispatch_status = DispatchStreamEvent(event_json, &summary, on_event);
        if (!dispatch_status.ok()) {
            return dispatch_status;
        }
        return true;
    };

    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const std::size_t next_newline = body.find('\n', cursor);
        const std::size_t line_end =
            next_newline == std::string_view::npos ? body.size() : next_newline;
        std::string line(body.substr(cursor, line_end - cursor));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            const absl::StatusOr<bool> flushed = flush_event();
            if (!flushed.ok()) {
                return flushed.status();
            }
        } else if (line.starts_with("data:")) {
            const std::string payload = TrimAscii(std::string_view(line).substr(5));
            if (!data.empty()) {
                data.push_back('\n');
            }
            data.append(payload);
        } else if (line.starts_with("event:")) {
            event_name = TrimAscii(std::string_view(line).substr(6));
        }

        if (next_newline == std::string_view::npos) {
            break;
        }
        cursor = next_newline + 1U;
    }

    const absl::StatusOr<bool> trailing_status = flush_event();
    if (!trailing_status.ok()) {
        return trailing_status.status();
    }
    if (!summary.saw_completed) {
        return internal_error("openai responses stream ended before completion");
    }
    return summary;
}

absl::StatusOr<std::string> ExecuteCurl(const OpenAiResponsesClientConfig& config,
                                        const std::string& request_json) {
    ScopedTempFile request_file(".json");
    ScopedTempFile header_file(".headers");

    {
        std::ofstream output(request_file.path(), std::ios::binary);
        output << request_json;
    }

    // NOTICE: The current Windows toolchain in this repository does not provide OpenSSL headers,
    // which blocks a direct Boost.Beast HTTPS implementation. This subprocess curl transport keeps
    // the OpenAI HTTP/SSE integration behind the provider boundary until the toolchain grows a
    // first-class TLS client dependency.
    std::ostringstream command;
    command << kCurlCommand << " --silent --show-error --no-buffer --http1.1 --request POST "
            << ShellQuote(BuildCurlUrl(config)) << " --header "
            << ShellQuote("Authorization: Bearer " + config.api_key) << " --header "
            << ShellQuote("Content-Type: application/json") << " --header "
            << ShellQuote("Accept: text/event-stream") << " --header "
            << ShellQuote("User-Agent: " + config.user_agent);
    if (config.organization.has_value()) {
        command << " --header " << ShellQuote("OpenAI-Organization: " + *config.organization);
    }
    if (config.project.has_value()) {
        command << " --header " << ShellQuote("OpenAI-Project: " + *config.project);
    }
    command << " --dump-header " << ShellQuote(header_file.path().string()) << " --data-binary "
            << ShellQuote("@" + request_file.path().string()) << " --max-time "
            << std::max<std::int64_t>(1, config.request_timeout.count() / 1000);

    PipeHandle pipe = OpenPipe(command.str());
    if (pipe == nullptr) {
        LOG(ERROR) << "AI gateway openai responses failed to launch curl host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "'";
        return absl::UnavailableError("failed to launch curl for openai responses request");
    }

    std::string body;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        body.append(buffer.data());
    }
    const int exit_code = ClosePipe(pipe);
    const std::string header_text = ReadFileToString(header_file.path());
    const std::optional<unsigned int> status_code = ParseHttpStatusCode(header_text);

    if (exit_code != 0) {
        LOG(ERROR) << "AI gateway openai responses curl transport failed host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' exit_code=" << exit_code << " http_status="
                   << (status_code.has_value() ? std::to_string(*status_code)
                                               : std::string("<none>"));
        if (status_code.has_value()) {
            return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body));
        }
        return absl::UnavailableError("openai responses transport command failed");
    }
    if (status_code.has_value() && *status_code != 200U) {
        return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body));
    }
    return body;
}

class CurlOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit CurlOpenAiResponsesClient(OpenAiResponsesClientConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] absl::Status Validate() const override {
        if (!config_.enabled) {
            return invalid_argument("openai responses client is disabled");
        }
        if (config_.api_key.empty()) {
            return invalid_argument("openai responses api_key must not be empty");
        }
        if (config_.host.empty()) {
            return invalid_argument("openai responses host must not be empty");
        }
        if (config_.target.empty() || config_.target.front() != '/') {
            return invalid_argument("openai responses target must start with '/'");
        }
        if (config_.scheme != "http" && config_.scheme != "https") {
            return invalid_argument("openai responses scheme must be 'http' or 'https'");
        }
        if (config_.request_timeout <= std::chrono::milliseconds::zero()) {
            return invalid_argument("openai responses request_timeout must be positive");
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        absl::Status status = Validate();
        if (!status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("openai responses request must include model");
        }
        if (request.user_text.empty()) {
            return invalid_argument("openai responses request must include user_text");
        }

        nlohmann::json body = {
            { "model", request.model },
            { "input", request.user_text },
            { "stream", true },
        };
        if (!request.system_prompt.empty()) {
            body["instructions"] = request.system_prompt;
        }

        VLOG(1) << "AI gateway openai responses dispatching host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target) << "' model='"
                << SanitizeForLog(request.model)
                << "' timeout_ms=" << config_.request_timeout.count()
                << " user_text_bytes=" << request.user_text.size()
                << " system_prompt_present=" << (!request.system_prompt.empty() ? "true" : "false");

        const absl::StatusOr<std::string> response_body = ExecuteCurl(config_, body.dump());
        if (!response_body.ok()) {
            LOG(ERROR) << "AI gateway openai responses request failed host='"
                       << SanitizeForLog(config_.host) << "' target='"
                       << SanitizeForLog(config_.target) << "' detail='"
                       << SanitizeForLog(response_body.status().message()) << "'";
            return response_body.status();
        }
        const absl::StatusOr<SseParseSummary> parse_summary =
            ParseSseBody(*response_body, on_event);
        if (!parse_summary.ok()) {
            return parse_summary.status();
        }
        VLOG(1) << "AI gateway openai responses completed host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target)
                << "' body_bytes=" << response_body->size()
                << " saw_delta=" << (parse_summary->saw_delta ? "true" : "false")
                << " saw_completed=" << (parse_summary->saw_completed ? "true" : "false")
                << " event_count=" << parse_summary->event_count;
        return absl::OkStatus();
    }

  private:
    OpenAiResponsesClientConfig config_;
};

} // namespace

std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config) {
    return std::make_shared<CurlOpenAiResponsesClient>(std::move(config));
}

} // namespace isla::server::ai_gateway
