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

TEST(PromptLoaderTest, DefaultSystemPromptMatchesLoadedSystemPrompt) {
    const absl::StatusOr<std::string> system_prompt = LoadSystemPrompt();

    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    EXPECT_EQ(DefaultSystemPrompt(), *system_prompt);
}

TEST(PromptLoaderTest, LoadPromptReturnsNotFoundForMissingAsset) {
    const absl::StatusOr<std::string> missing_prompt =
        LoadPrompt("server/memory/include/prompts/does_not_exist.txt");

    ASSERT_FALSE(missing_prompt.ok());
    EXPECT_EQ(missing_prompt.status().code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace isla::server::memory
