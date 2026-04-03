#include "isla/server/jina_api_reranker_client.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
using nlohmann::json;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

std::string BuildBaseUrl(const JinaApiRerankerClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port);
}

std::string NormalizeTarget(std::string_view target) {
    if (target.front() == '/') {
        return std::string(target);
    }
    return "/" + std::string(target);
}

json BuildRerankRequestBody(const RerankRequest& request) {
    return json{
        { "model", request.model },
        { "query", request.query },
        { "documents", json(request.candidates) },
        { "top_n", request.candidates.size() },
    };
}

std::string ExtractJinaApiErrorDetail(std::string_view body) {
    if (body.empty()) {
        return "empty response body";
    }
    const json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_object()) {
        return std::string(body);
    }
    if (const auto detail_it = parsed.find("detail");
        detail_it != parsed.end() && detail_it->is_string()) {
        return detail_it->get<std::string>();
    }
    if (const auto message_it = parsed.find("message");
        message_it != parsed.end() && message_it->is_string()) {
        return message_it->get<std::string>();
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
    return std::string(body);
}

absl::Status MapJinaApiHttpError(unsigned int status_code, std::string_view body) {
    const std::string detail = ExtractJinaApiErrorDetail(body);
    switch (status_code) {
    case 400:
    case 422:
        return absl::InvalidArgumentError(detail);
    case 401:
        return absl::UnauthenticatedError(detail);
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

absl::StatusOr<std::vector<double>> ParseRerankResponse(std::string_view response_body,
                                                        std::size_t expected_count) {
    json parsed;
    try {
        parsed = json::parse(response_body);
    } catch (const std::exception& error) {
        return invalid_argument(std::string("jina api reranker response contained invalid JSON: ") +
                                error.what());
    }
    if (!parsed.is_object()) {
        return invalid_argument("jina api reranker response must be a JSON object");
    }
    if (!parsed.contains("results") || !parsed.at("results").is_array()) {
        return invalid_argument("jina api reranker response must contain a results array");
    }

    std::vector<double> scores(expected_count, 0.0);
    std::vector<bool> seen(expected_count, false);
    for (const json& result : parsed.at("results")) {
        if (!result.is_object()) {
            return invalid_argument("jina api reranker results must contain only objects");
        }
        if (!result.contains("index") || !result.at("index").is_number_integer()) {
            return invalid_argument(
                "jina api reranker result entries must contain an integer index");
        }
        if (!result.contains("relevance_score") || !result.at("relevance_score").is_number()) {
            return invalid_argument(
                "jina api reranker result entries must contain a numeric relevance_score");
        }
        const std::int64_t raw_index = result.at("index").get<std::int64_t>();
        if (raw_index < 0) {
            return invalid_argument("jina api reranker returned a negative result index");
        }
        const std::size_t index = static_cast<std::size_t>(raw_index);
        if (index >= expected_count) {
            return invalid_argument("jina api reranker returned an out-of-range result index");
        }
        const double score = result.at("relevance_score").get<double>();
        if (!std::isfinite(score)) {
            return invalid_argument("jina api reranker returned a non-finite relevance_score");
        }
        if (seen[index]) {
            return invalid_argument("jina api reranker returned a duplicate result index");
        }
        seen[index] = true;
        scores[index] = score;
    }

    for (bool present : seen) {
        if (!present) {
            return invalid_argument(
                "jina api reranker response did not include scores for every candidate");
        }
    }
    return scores;
}

class JinaApiRerankerClient final : public RerankerClient {
  public:
    JinaApiRerankerClient(JinaApiRerankerClientConfig config,
                          std::unique_ptr<PersistentHttpClient> http_client)
        : config_(std::move(config)), http_client_(std::move(http_client)) {}

    [[nodiscard]] absl::Status Validate() const override {
        return ValidateJinaApiRerankerClientConfig(config_);
    }

    [[nodiscard]] absl::Status WarmUp() const override {
        return http_client_->WarmUp();
    }

    [[nodiscard]] absl::StatusOr<std::vector<double>>
    Rerank(const RerankRequest& request) const override {
        if (absl::Status status = Validate(); !status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("rerank request model must not be empty");
        }
        if (request.query.empty()) {
            return invalid_argument("rerank request query must not be empty");
        }
        if (request.candidates.empty()) {
            return invalid_argument("rerank request candidates must not be empty");
        }

        const HttpRequestSpec http_request{
            .method = boost::beast::http::verb::post,
            .target_path = NormalizeTarget(config_.target),
            .headers =
                {
                    { "Authorization", "Bearer " + config_.api_key },
                    { "Accept", "application/json" },
                    { "Content-Type", "application/json" },
                },
            .body = BuildRerankRequestBody(request).dump(),
        };

        VLOG(1) << "JinaApiRerankerClient dispatching host='" << SanitizeForLog(config_.host)
                << "' model='" << SanitizeForLog(request.model)
                << "' candidates=" << request.candidates.size();

        const absl::StatusOr<HttpResponse> response = http_client_->Execute(http_request);
        if (!response.ok()) {
            LOG(WARNING) << "JinaApiRerankerClient transport failed host='"
                         << SanitizeForLog(config_.host) << "' model='"
                         << SanitizeForLog(request.model) << "' detail='"
                         << SanitizeForLog(response.status().message()) << "'";
            return response.status();
        }
        if (response->status_code < 200U || response->status_code >= 300U) {
            absl::Status status = MapJinaApiHttpError(response->status_code, response->body);
            LOG(WARNING) << "JinaApiRerankerClient request failed host='"
                         << SanitizeForLog(config_.host) << "' model='"
                         << SanitizeForLog(request.model)
                         << "' status_code=" << response->status_code << " detail='"
                         << SanitizeForLog(status.message()) << "'";
            return status;
        }
        return ParseRerankResponse(response->body, request.candidates.size());
    }

  private:
    JinaApiRerankerClientConfig config_;
    std::unique_ptr<PersistentHttpClient> http_client_;
};

} // namespace

absl::Status ValidateJinaApiRerankerClientConfig(const JinaApiRerankerClientConfig& config) {
    if (!config.enabled) {
        return absl::OkStatus();
    }
    if (config.api_key.empty()) {
        return absl::InvalidArgumentError(
            "jina api key must not be empty when reranking is enabled");
    }
    if (config.scheme != "http" && config.scheme != "https") {
        return absl::InvalidArgumentError("jina api scheme must be 'http' or 'https'");
    }
    if (config.host.empty()) {
        return absl::InvalidArgumentError(
            "jina api host must not be empty when reranking is enabled");
    }
    if (config.target.empty()) {
        return absl::InvalidArgumentError(
            "jina api target must not be empty when reranking is enabled");
    }
    if (config.request_timeout <= std::chrono::milliseconds::zero()) {
        return absl::InvalidArgumentError("jina api timeout must be positive");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<const RerankerClient>>
CreateJinaApiRerankerClient(JinaApiRerankerClientConfig config) {
    if (!config.enabled) {
        return std::shared_ptr<const RerankerClient>{};
    }
    if (absl::Status status = ValidateJinaApiRerankerClientConfig(config); !status.ok()) {
        return status;
    }
    const absl::StatusOr<ParsedHttpUrl> parsed_url =
        ParseHttpUrl(BuildBaseUrl(config), "jina api reranker url");
    if (!parsed_url.ok()) {
        return parsed_url.status();
    }
#if defined(_WIN32)
    if (parsed_url->scheme == "https") {
        return absl::FailedPreconditionError("jina api https transport is unavailable in "
                                             "Windows builds; run the gateway server on Linux");
    }
#endif
    const HttpClientConfig http_config{
        .request_timeout = config.request_timeout,
        .user_agent = config.user_agent,
        .trusted_ca_cert_pem = config.trusted_ca_cert_pem,
    };
    auto http_client = std::make_unique<PersistentHttpClient>(*parsed_url, http_config);
    return std::shared_ptr<const RerankerClient>(
        std::make_shared<JinaApiRerankerClient>(std::move(config), std::move(http_client)));
}

} // namespace isla::server
