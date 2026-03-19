#include "isla/server/gemini_api_embedding_client.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_json_client.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server {
namespace {

using isla::server::HttpClientConfig;
using isla::server::HttpRequestSpec;
using isla::server::HttpResponse;
using isla::server::ParsedHttpUrl;
using isla::server::ParseHttpUrl;
using isla::server::PersistentHttpClient;
using isla::server::ai_gateway::SanitizeForLog;
using isla::server::memory::Embedding;
using nlohmann::json;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string BuildBaseUrl(const GeminiApiEmbeddingClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port);
}

std::string BuildTargetPath(std::string_view model) {
    return "/v1beta/models/" + std::string(model) + ":embedContent";
}

json BuildEmbeddingRequestBody(std::string_view text) {
    return json{
        { "content",
          json{
              { "parts", json::array({ json{ { "text", std::string(text) } } }) },
          } },
    };
}

std::string ExtractGeminiApiErrorDetail(std::string_view body) {
    if (body.empty()) {
        return "empty response body";
    }
    const json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_object()) {
        return std::string(body);
    }
    if (const auto error_it = parsed.find("error"); error_it != parsed.end()) {
        if (error_it->is_string()) {
            return error_it->get<std::string>();
        }
        if (error_it->is_object()) {
            if (const auto message_it = error_it->find("message");
                message_it != error_it->end() && message_it->is_string()) {
                return message_it->get<std::string>();
            }
        }
    }
    if (const auto message_it = parsed.find("message");
        message_it != parsed.end() && message_it->is_string()) {
        return message_it->get<std::string>();
    }
    return std::string(body);
}

absl::Status MapGeminiApiHttpError(unsigned int status_code, std::string_view body) {
    const std::string detail = ExtractGeminiApiErrorDetail(body);
    switch (status_code) {
    case 400:
    case 422:
        return absl::InvalidArgumentError(detail);
    case 401:
    case 403:
        return absl::PermissionDeniedError(detail);
    case 404:
        return absl::NotFoundError(detail);
    case 429:
        return absl::ResourceExhaustedError(detail);
    default:
        if (status_code >= 500U) {
            return absl::UnavailableError(detail);
        }
        return absl::UnknownError(detail);
    }
}

absl::StatusOr<Embedding> ParseEmbeddingResponse(std::string_view response_body) {
    json parsed;
    try {
        parsed = json::parse(response_body);
    } catch (const std::exception& error) {
        return invalid_argument(std::string("gemini api embedding response contained invalid "
                                            "JSON: ") +
                                error.what());
    }
    if (!parsed.is_object()) {
        return invalid_argument("gemini api embedding response must be a JSON object");
    }
    if (!parsed.contains("embedding") || !parsed.at("embedding").is_object()) {
        return invalid_argument("gemini api embedding response must contain an embedding object");
    }
    const json& embedding = parsed.at("embedding");
    if (!embedding.contains("values") || !embedding.at("values").is_array()) {
        return invalid_argument(
            "gemini api embedding response embedding must contain a values array");
    }

    Embedding result;
    result.reserve(embedding.at("values").size());
    for (const json& value : embedding.at("values")) {
        if (!value.is_number()) {
            return invalid_argument(
                "gemini api embedding response values must contain only numbers");
        }
        result.push_back(value.get<double>());
    }
    if (result.empty()) {
        return invalid_argument("gemini api embedding response returned an empty embedding");
    }
    return result;
}

class GeminiApiEmbeddingClient final : public EmbeddingClient {
  public:
    GeminiApiEmbeddingClient(GeminiApiEmbeddingClientConfig config,
                             std::unique_ptr<PersistentHttpClient> http_client)
        : config_(std::move(config)), http_client_(std::move(http_client)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return ValidateGeminiApiEmbeddingClientConfig(config_);
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return http_client_->WarmUp();
    }

    [[nodiscard]] absl::StatusOr<Embedding>
    Embed(const EmbeddingRequest& request) const override {
        if (absl::Status status = Validate(); !status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("embedding request model must not be empty");
        }
        if (request.text.empty()) {
            return invalid_argument("embedding request text must not be empty");
        }

        const HttpRequestSpec http_request{
            .method = boost::beast::http::verb::post,
            .target_path = BuildTargetPath(request.model),
            .headers =
                {
                    { "x-goog-api-key", config_.api_key },
                    { "Accept", "application/json" },
                },
            .body = BuildEmbeddingRequestBody(request.text).dump(),
        };

        VLOG(1) << "GeminiApiEmbeddingClient dispatching host='"
                << SanitizeForLog(config_.host) << "' model='"
                << SanitizeForLog(request.model) << "'";

        const absl::StatusOr<HttpResponse> response = http_client_->Execute(http_request);
        if (!response.ok()) {
            LOG(WARNING) << "GeminiApiEmbeddingClient transport failed host='"
                         << SanitizeForLog(config_.host) << "' model='"
                         << SanitizeForLog(request.model) << "' detail='"
                         << SanitizeForLog(response.status().message()) << "'";
            return response.status();
        }
        if (response->status_code < 200U || response->status_code >= 300U) {
            const absl::Status status = MapGeminiApiHttpError(response->status_code, response->body);
            LOG(WARNING) << "GeminiApiEmbeddingClient request failed host='"
                         << SanitizeForLog(config_.host) << "' model='"
                         << SanitizeForLog(request.model) << "' status_code="
                         << response->status_code << " detail='"
                         << SanitizeForLog(status.message()) << "'";
            return status;
        }

        return ParseEmbeddingResponse(response->body);
    }

  private:
    GeminiApiEmbeddingClientConfig config_;
    std::unique_ptr<PersistentHttpClient> http_client_;
};

} // namespace

absl::Status ValidateGeminiApiEmbeddingClientConfig(const GeminiApiEmbeddingClientConfig& config) {
    if (!config.enabled) {
        return absl::OkStatus();
    }
    if (config.api_key.empty()) {
        return absl::InvalidArgumentError(
            "gemini api key must not be empty when embeddings are enabled");
    }
    if (config.scheme != "http" && config.scheme != "https") {
        return absl::InvalidArgumentError("gemini api scheme must be 'http' or 'https'");
    }
    if (config.host.empty()) {
        return absl::InvalidArgumentError(
            "gemini api host must not be empty when embeddings are enabled");
    }
    if (config.request_timeout <= std::chrono::milliseconds::zero()) {
        return absl::InvalidArgumentError("gemini api timeout must be positive");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<const EmbeddingClient>>
CreateGeminiApiEmbeddingClient(GeminiApiEmbeddingClientConfig config) {
    if (!config.enabled) {
        return std::shared_ptr<const EmbeddingClient>{};
    }
    if (absl::Status status = ValidateGeminiApiEmbeddingClientConfig(config); !status.ok()) {
        return status;
    }
    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl(BuildBaseUrl(config), "gemini api embedding url");
    if (!parsed_url.ok()) {
        return parsed_url.status();
    }
#if defined(_WIN32)
    if (parsed_url->scheme == "https") {
        return absl::FailedPreconditionError("gemini api https transport is unavailable in "
                                             "Windows builds; run the gateway server on Linux");
    }
#endif
    const HttpClientConfig http_config{
        .request_timeout = config.request_timeout,
        .user_agent = config.user_agent,
        .trusted_ca_cert_pem = config.trusted_ca_cert_pem,
    };
    auto http_client = std::make_unique<PersistentHttpClient>(*parsed_url, http_config);
    return std::shared_ptr<const EmbeddingClient>(
        std::make_shared<GeminiApiEmbeddingClient>(std::move(config), std::move(http_client)));
}

} // namespace isla::server
