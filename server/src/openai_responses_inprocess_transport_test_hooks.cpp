#include "openai_responses_inprocess_transport_test_hooks.hpp"

#include <cstdint>
#include <memory>

namespace isla::server::ai_gateway {

std::uint64_t RegisterOpenAiResponsesHostResolverOverrideForTest(
    std::shared_ptr<const OpenAiResponsesHostResolver> resolver);
void UnregisterOpenAiResponsesHostResolverOverrideForTest(std::uint64_t token);

ScopedOpenAiResponsesHostResolverOverrideForTest::ScopedOpenAiResponsesHostResolverOverrideForTest(
    std::shared_ptr<const OpenAiResponsesHostResolver> resolver)
    : token_(RegisterOpenAiResponsesHostResolverOverrideForTest(std::move(resolver))) {}

ScopedOpenAiResponsesHostResolverOverrideForTest::
    ~ScopedOpenAiResponsesHostResolverOverrideForTest() {
    if (token_ != 0U) {
        UnregisterOpenAiResponsesHostResolverOverrideForTest(token_);
    }
}

} // namespace isla::server::ai_gateway
