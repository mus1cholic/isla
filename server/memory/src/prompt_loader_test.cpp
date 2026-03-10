#include "isla/server/memory/prompt_loader.hpp"

#include <gtest/gtest.h>

namespace isla::server::memory {
namespace {

inline constexpr std::string_view kFuturePromptTestRunfile =
    "server/memory/include/prompts/future_prompt_test.txt";

TEST(PromptLoaderTest, LoadPromptReadsSystemPromptAsset) {
    const absl::StatusOr<std::string> generic_prompt = LoadPrompt(kSystemPromptRunfile);
    const absl::StatusOr<std::string> system_prompt = LoadSystemPrompt();

    ASSERT_TRUE(generic_prompt.ok()) << generic_prompt.status();
    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_FALSE(generic_prompt->empty());
    EXPECT_EQ(*generic_prompt, *system_prompt);
}

TEST(PromptLoaderTest, LoadPromptReadsNonSystemPromptAsset) {
    const absl::StatusOr<std::string> prompt = LoadPrompt(kFuturePromptTestRunfile);

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

TEST(PromptLoaderTest, LoadPromptReturnsNotFoundForMissingAsset) {
    const absl::StatusOr<std::string> missing_prompt =
        LoadPrompt("server/memory/include/prompts/does_not_exist.txt");

    ASSERT_FALSE(missing_prompt.ok());
    EXPECT_EQ(missing_prompt.status().code(), absl::StatusCode::kNotFound);
}

TEST(PromptLoaderTest, LoadPromptDoesNotReadTraversalOrAbsoluteLikePaths) {
    const absl::StatusOr<std::string> traversal_prompt =
        LoadPrompt("../memory/include/prompts/system_prompt.txt");
    const absl::StatusOr<std::string> absolute_like_prompt =
        LoadPrompt("C:/secret/system_prompt.txt");

    ASSERT_FALSE(traversal_prompt.ok());
    EXPECT_EQ(traversal_prompt.status().code(), absl::StatusCode::kNotFound);
    ASSERT_FALSE(absolute_like_prompt.ok());
    EXPECT_EQ(absolute_like_prompt.status().code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace isla::server::memory
