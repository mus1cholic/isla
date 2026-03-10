#include "isla/server/memory/prompt_loader.hpp"

#include <gtest/gtest.h>

namespace isla::server::memory {
namespace {

TEST(PromptLoaderTest, LoadPromptReadsSystemPromptAsset) {
    const absl::StatusOr<std::string> generic_prompt = LoadPrompt(PromptAsset::kSystemPrompt);
    const absl::StatusOr<std::string> system_prompt = LoadSystemPrompt();

    ASSERT_TRUE(generic_prompt.ok()) << generic_prompt.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_FALSE(generic_prompt->empty());
    EXPECT_EQ(*generic_prompt, *system_prompt);
}

TEST(PromptLoaderTest, LoadPromptReadsNonSystemPromptAsset) {
    const absl::StatusOr<std::string> prompt = LoadPrompt(PromptAsset::kFuturePromptTest);

    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_EQ(*prompt,
              "Future prompt fixture.\nUse this file to validate generic prompt loading.\n");
}

TEST(PromptLoaderTest, ResolveSystemPromptUsesEmbeddedDefaultWhenConfigIsEmpty) {
    const absl::StatusOr<std::string> resolved_prompt = ResolveSystemPrompt("");
    const absl::StatusOr<std::string> system_prompt = LoadSystemPrompt();

    ASSERT_TRUE(resolved_prompt.ok()) << resolved_prompt.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_EQ(*resolved_prompt, *system_prompt);
}

TEST(PromptLoaderTest, ResolveSystemPromptPreservesExplicitPrompt) {
    const absl::StatusOr<std::string> resolved_prompt = ResolveSystemPrompt("configured prompt");

    ASSERT_TRUE(resolved_prompt.ok()) << resolved_prompt.status();
    EXPECT_EQ(*resolved_prompt, "configured prompt");
}

TEST(PromptLoaderTest, LoadPromptRejectsUnknownPromptAsset) {
    const absl::StatusOr<std::string> missing_prompt = LoadPrompt(static_cast<PromptAsset>(999));

    ASSERT_FALSE(missing_prompt.ok());
    EXPECT_EQ(missing_prompt.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace isla::server::memory
