#pragma once

#include <memory>

#include "openai_responses_inprocess_transport_resolver.hpp"

namespace isla::server::ai_gateway {

// Test-only process-wide DNS override for the in-process OpenAI transport.
// The active override affects all OpenAI responses clients in the current process and is not
// scoped to a specific client instance. Use ScopedOpenAiResponsesHostResolverOverrideForTest to
// install one only inside tightly-bounded test code.
class ScopedOpenAiResponsesHostResolverOverrideForTest {
  public:
    explicit ScopedOpenAiResponsesHostResolverOverrideForTest(
        std::shared_ptr<const OpenAiResponsesHostResolver> resolver);
    ~ScopedOpenAiResponsesHostResolverOverrideForTest();

    ScopedOpenAiResponsesHostResolverOverrideForTest(
        const ScopedOpenAiResponsesHostResolverOverrideForTest&) = delete;
    ScopedOpenAiResponsesHostResolverOverrideForTest&
    operator=(const ScopedOpenAiResponsesHostResolverOverrideForTest&) = delete;

  private:
    std::uint64_t token_ = 0;
};

} // namespace isla::server::ai_gateway
