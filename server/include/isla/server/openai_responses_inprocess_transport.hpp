#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/openai_responses_client.hpp"
#include "isla/server/openai_responses_transport_utils.hpp"

namespace isla::server::ai_gateway {

absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttp(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                     const OpenAiResponsesEventCallback& on_event);

#if !defined(_WIN32)
absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttps(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                      const OpenAiResponsesEventCallback& on_event);
#endif

// Maintains a persistent HTTP(S) connection to the OpenAI API host for reuse
// across SSE streaming requests. Thread-safe via internal mutex. The owner must
// ensure this object outlives all concurrent Execute() calls; destroying it
// while a call is in-flight is undefined behavior that cannot be fixed by
// locking the destructor.
class PersistentInProcessTransport {
  public:
    explicit PersistentInProcessTransport(OpenAiResponsesClientConfig config);
    ~PersistentInProcessTransport();

    PersistentInProcessTransport(const PersistentInProcessTransport&) = delete;
    PersistentInProcessTransport& operator=(const PersistentInProcessTransport&) = delete;

    [[nodiscard]] absl::StatusOr<TransportStreamResult>
    Execute(const std::string& request_json, const OpenAiResponsesEventCallback& on_event);

    // Eagerly establishes the underlying TCP/TLS connection so that the first
    // Execute() call does not pay the connection-setup latency. Safe to call
    // multiple times; subsequent calls are no-ops while the connection is alive.
    [[nodiscard]] absl::Status WarmUp();

  private:
    absl::Status EnsureConnected();
    void Disconnect();
    absl::StatusOr<TransportStreamResult> ExecuteOnce(const std::string& request_json,
                                                      const OpenAiResponsesEventCallback& on_event,
                                                      bool* request_written,
                                                      bool* server_keep_alive);

    struct Impl;
    OpenAiResponsesClientConfig config_;
    std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::server::ai_gateway
