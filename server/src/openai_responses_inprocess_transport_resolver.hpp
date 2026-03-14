#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"

namespace isla::server::ai_gateway {

class OpenAiResponsesHostResolver {
  public:
    virtual ~OpenAiResponsesHostResolver() = default;

    [[nodiscard]] virtual absl::StatusOr<boost::asio::ip::tcp::resolver::results_type>
    Resolve(boost::asio::io_context* io_context, const OpenAiResponsesClientConfig& config,
            std::chrono::steady_clock::time_point deadline) const = 0;
};

std::shared_ptr<const OpenAiResponsesHostResolver> GetOpenAiResponsesHostResolver();

} // namespace isla::server::ai_gateway
