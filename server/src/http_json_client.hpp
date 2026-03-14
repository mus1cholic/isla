#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/beast/http/verb.hpp>

#include "absl/status/statusor.h"

namespace isla::server {

struct ParsedHttpUrl {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string base_path;
};

struct HttpClientConfig {
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(10) };
    std::string user_agent;
    std::optional<std::string> trusted_ca_cert_pem;
};

struct HttpRequestSpec {
    boost::beast::http::verb method = boost::beast::http::verb::get;
    std::string target_path;
    std::vector<std::pair<std::string, std::string>> query_parameters;
    std::vector<std::pair<std::string, std::string>> headers;
    std::optional<std::string> body;
};

struct HttpResponse {
    unsigned int status_code = 0;
    std::string body;
};

[[nodiscard]] absl::StatusOr<ParsedHttpUrl> ParseHttpUrl(std::string_view url,
                                                         std::string_view url_label);
[[nodiscard]] std::string
BuildHttpQueryString(const std::vector<std::pair<std::string, std::string>>& query_parameters);
[[nodiscard]] absl::StatusOr<HttpResponse>
ExecuteHttpRequest(const ParsedHttpUrl& parsed_url, const HttpClientConfig& config,
                   const HttpRequestSpec& request);

} // namespace isla::server
