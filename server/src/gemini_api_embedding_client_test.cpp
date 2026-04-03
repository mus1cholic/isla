#include "isla/server/gemini_api_embedding_client.hpp"
#include "one_shot_http_server_test_utils.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server {
namespace {

using nlohmann::json;
using namespace std::chrono_literals;
using test::OneShotHttpServer;

GeminiApiEmbeddingClientConfig MakeConfig(std::uint16_t port) {
    return GeminiApiEmbeddingClientConfig{
        .enabled = true,
        .api_key = "api_key_test",
        .scheme = "http",
        .host = "127.0.0.1",
        .port = port,
        .request_timeout = 2s,
    };
}

json MakeEmbeddingValuesJson(std::size_t dimensions, double value) {
    json values = json::array();
    for (std::size_t index = 0; index < dimensions; ++index) {
        values.push_back(value);
    }
    return values;
}

std::string ExpectedRequestedDimensionText() {
    return "expected " + std::to_string(memory::kEmbeddingDimensions);
}

TEST(GeminiApiEmbeddingClientTest, ValidateRejectsMissingApiKey) {
    const absl::Status status =
        ValidateGeminiApiEmbeddingClientConfig(GeminiApiEmbeddingClientConfig{
            .enabled = true,
            .api_key = "",
        });

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(GeminiApiEmbeddingClientTest, EmbedPostsEmbedContentRequestAndParsesEmbedding) {
    const std::string response_body =
        json{
            { "embedding",
              json{ { "values", MakeEmbeddingValuesJson(memory::kEmbeddingDimensions, 0.25) } } },
        }
            .dump();
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();
    ASSERT_NE(*client, nullptr);

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_TRUE(embedding.ok()) << embedding.status();
    ASSERT_EQ(embedding->size(), memory::kEmbeddingDimensions);
    EXPECT_NEAR((*embedding)[0], 1.0 / std::sqrt(static_cast<double>(memory::kEmbeddingDimensions)),
                1e-6);
    double norm = 0.0;
    for (const double value : *embedding) {
        norm += value * value;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0, 1e-6);
    ASSERT_TRUE(server.WaitForRequest());
    const std::string request = server.request_text();
    EXPECT_NE(request.find("POST /v1beta/models/gemini-embedding-2-preview:embedContent HTTP/1.1"),
              std::string::npos);
    EXPECT_NE(request.find("x-goog-api-key: api_key_test"), std::string::npos);
    const std::size_t body_pos = request.find("\r\n\r\n");
    ASSERT_NE(body_pos, std::string::npos);
    const json body = json::parse(request.substr(body_pos + 4U));
    ASSERT_TRUE(body.contains("content"));
    ASSERT_TRUE(body.contains("output_dimensionality"));
    ASSERT_TRUE(body["content"].contains("parts"));
    ASSERT_EQ(body["content"]["parts"].size(), 1U);
    EXPECT_EQ(body["content"]["parts"][0]["text"], "debugged the export crash");
    EXPECT_EQ(body["output_dimensionality"], memory::kEmbeddingDimensions);
}

TEST(GeminiApiEmbeddingClientTest, EmbedRejectsUnexpectedOutputDimension) {
    const std::string response_body = R"({"embedding":{"values":[0.25,-0.5,1.75]}})";
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(embedding.status().message()).find(ExpectedRequestedDimensionText()),
              std::string::npos);
}

TEST(GeminiApiEmbeddingClientTest, EmbedRejectsZeroNormEmbeddingDuringNormalization) {
    const std::string response_body =
        json{
            { "embedding",
              json{ { "values", MakeEmbeddingValuesJson(memory::kEmbeddingDimensions, 0.0) } } },
        }
            .dump();
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(embedding.status().message()).find("zero-norm"), std::string::npos);
}

TEST(GeminiApiEmbeddingClientTest, EmbedRejectsNonFiniteEmbeddingNormDuringNormalization) {
    const std::string response_body =
        json{
            { "embedding",
              json{
                  { "values", MakeEmbeddingValuesJson(memory::kEmbeddingDimensions, 1e308) },
              } },
        }
            .dump();
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(embedding.status().message()).find("non-finite embedding norm"),
              std::string::npos);
}

TEST(GeminiApiEmbeddingClientTest, EmbedMapsHttpFailures) {
    const std::string response_body = R"({"error":{"message":"quota exceeded"}})";
    OneShotHttpServer server(
        "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(response_body.size()) + "\r\n\r\n" + response_body);
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_NE(std::string(embedding.status().message()).find("quota exceeded"), std::string::npos);
}

TEST(GeminiApiEmbeddingClientTest, EmbedRejectsMalformedResponses) {
    OneShotHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: 2\r\n\r\n{}");
    const absl::StatusOr<std::shared_ptr<const EmbeddingClient>> client =
        CreateGeminiApiEmbeddingClient(MakeConfig(server.port()));
    ASSERT_TRUE(client.ok()) << client.status();

    const absl::StatusOr<memory::Embedding> embedding = (*client)->Embed(EmbeddingRequest{
        .model = "gemini-embedding-2-preview",
        .text = "debugged the export crash",
        .output_dimensionality = memory::kEmbeddingDimensions,
    });

    ASSERT_FALSE(embedding.ok());
    EXPECT_EQ(embedding.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server
