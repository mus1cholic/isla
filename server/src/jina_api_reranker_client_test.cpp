#include "isla/server/jina_api_reranker_client.hpp"
#include "one_shot_http_server_test_utils.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server {
namespace {

using nlohmann::json;
using namespace std::chrono_literals;
using test::OneShotHttpServer;

JinaApiRerankerClientConfig MakeConfig(std::uint16_t port) {
    return JinaApiRerankerClientConfig{
        .enabled = true,
        .api_key = "jina_key_test",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = port,
        .request_timeout = 2s,
    };
}

TEST(JinaApiRerankerClientTest, ValidateRejectsMissingApiKey) {
    const absl::Status status = ValidateJinaApiRerankerClientConfig(JinaApiRerankerClientConfig{
        .enabled = true,
        .api_key = "",
    });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(JinaApiRerankerClientTest, RerankPostsRequestAndParsesScoresByIndex) {
    const std::string response_body =
        json{
            { "results", json::array({
                             json{ { "index", 1 }, { "relevance_score", 0.25 } },
                             json{ { "index", 0 }, { "relevance_score", 0.75 } },
                         }) },
        }
            .dump();
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const RerankerClient>> client =
        CreateJinaApiRerankerClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();
    ASSERT_NE(*client, nullptr);

    const absl::StatusOr<std::vector<double>> scores = (*client)->Rerank(RerankRequest{
        .model = "jina-reranker-v3",
        .query = "Tell me about Mochi",
        .candidates = { "Airi owns Mochi", "Airi likes tea" },
        .telemetry_context = nullptr,
    });

    ASSERT_TRUE(scores.ok()) << scores.status();
    ASSERT_EQ(scores->size(), 2U);
    EXPECT_DOUBLE_EQ((*scores)[0], 0.75);
    EXPECT_DOUBLE_EQ((*scores)[1], 0.25);

    ASSERT_TRUE(server.WaitForRequest());
    const std::string request = server.request_text();
    EXPECT_NE(request.find("POST /v1/rerank HTTP/1.1"), std::string::npos);
    EXPECT_NE(request.find("Authorization: Bearer jina_key_test"), std::string::npos);
    const std::size_t body_pos = request.find("\r\n\r\n");
    ASSERT_NE(body_pos, std::string::npos);
    const json body = json::parse(request.substr(body_pos + 4U));
    EXPECT_EQ(body["model"], "jina-reranker-v3");
    EXPECT_EQ(body["query"], "Tell me about Mochi");
    EXPECT_EQ(body["top_n"], 2);
    ASSERT_TRUE(body["documents"].is_array());
    ASSERT_EQ(body["documents"].size(), 2U);
    EXPECT_EQ(body["documents"][0], "Airi owns Mochi");
    EXPECT_EQ(body["documents"][1], "Airi likes tea");
}

TEST(JinaApiRerankerClientTest, RerankMapsHttpFailures) {
    const std::string response_body = R"({"detail":"rate limit hit"})";
    OneShotHttpServer server(
        "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const RerankerClient>> client =
        CreateJinaApiRerankerClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<std::vector<double>> scores = (*client)->Rerank(RerankRequest{
        .model = "jina-reranker-v3",
        .query = "Tell me about Mochi",
        .candidates = { "Airi owns Mochi" },
        .telemetry_context = nullptr,
    });

    ASSERT_FALSE(scores.ok());
    EXPECT_EQ(scores.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_NE(std::string(scores.status().message()).find("rate limit hit"), std::string::npos);
}

TEST(JinaApiRerankerClientTest, RerankRejectsMalformedResponses) {
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: 2\r\n\r\n{}");
    const absl::StatusOr<std::shared_ptr<const RerankerClient>> client =
        CreateJinaApiRerankerClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<std::vector<double>> scores = (*client)->Rerank(RerankRequest{
        .model = "jina-reranker-v3",
        .query = "Tell me about Mochi",
        .candidates = { "Airi owns Mochi" },
        .telemetry_context = nullptr,
    });

    ASSERT_FALSE(scores.ok());
    EXPECT_EQ(scores.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server
