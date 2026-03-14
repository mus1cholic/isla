#include "isla/server/memory/prompt_loader.hpp"
#include "isla/server/memory/system_prompt.hpp"
#include "isla/server/memory/working_memory_utils.hpp"

#include <string>

#include <gtest/gtest.h>

namespace isla::server::memory {
namespace {

TEST(SystemPromptTest, CreateUsesBundledPromptWhenConfigIsEmpty) {
    const absl::StatusOr<SystemPrompt> system_prompt = SystemPrompt::Create("");
    const absl::StatusOr<std::string> bundled_prompt = LoadSystemPrompt();

    ASSERT_TRUE(system_prompt.ok()) << system_prompt.status();
    ASSERT_TRUE(bundled_prompt.ok()) << bundled_prompt.status();
    EXPECT_EQ(system_prompt->snapshot().base_instructions, *bundled_prompt);
    EXPECT_TRUE(system_prompt->snapshot().persistent_memory_cache.active_models.empty());
    EXPECT_TRUE(system_prompt->snapshot().persistent_memory_cache.familiar_labels.empty());
}

TEST(SystemPromptTest, RenderIncludesPersistentMemoryCacheSection) {
    const absl::StatusOr<SystemPrompt> created = SystemPrompt::Create("You are Isla.");

    ASSERT_TRUE(created.ok()) << created.status();
    SystemPrompt system_prompt = *created;
    UpsertActiveModel(system_prompt.mutable_persistent_memory_cache(), "entity_user",
                      "Airi, the user.");
    UpsertFamiliarLabel(system_prompt.mutable_persistent_memory_cache(), "entity_mochi",
                        "Airi's cat");

    const absl::StatusOr<std::string> rendered = system_prompt.Render();

    ASSERT_TRUE(rendered.ok()) << rendered.status();
    EXPECT_EQ(rendered->compare(0, std::string("You are Isla.").size(), "You are Isla."), 0);
    EXPECT_NE(rendered->find("<persistent_memory_cache>"), std::string::npos);
    EXPECT_NE(rendered->find("Active Models:"), std::string::npos);
    EXPECT_NE(rendered->find("- [entity_user] Airi, the user."), std::string::npos);
    EXPECT_NE(rendered->find("Familiar Labels:"), std::string::npos);
    EXPECT_NE(rendered->find("- [entity_mochi] Airi's cat"), std::string::npos);
}

} // namespace
} // namespace isla::server::memory
