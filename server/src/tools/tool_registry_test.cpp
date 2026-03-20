#include "isla/server/tools/tool_registry.hpp"

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace isla::server::tools {
namespace {

class StaticTool final : public Tool {
  public:
    StaticTool(ToolDefinition definition, ToolResult result)
        : definition_(std::move(definition)), result_(std::move(result)) {}

    [[nodiscard]] ToolDefinition Definition() const override {
        return definition_;
    }

    [[nodiscard]] absl::StatusOr<ToolResult> Execute(const ToolExecutionContext& context,
                                                     const ToolCall& call) const override {
        static_cast<void>(context);
        static_cast<void>(call);
        return result_;
    }

  private:
    ToolDefinition definition_;
    ToolResult result_;
};

TEST(ToolRegistryTest, CreateRejectsDuplicateNames) {
    const auto first = std::make_shared<StaticTool>(
        ToolDefinition{
            .name = "duplicate",
            .description = "first",
            .input_json_schema = "{}",
        },
        ToolResult{});
    const auto second = std::make_shared<StaticTool>(
        ToolDefinition{
            .name = "duplicate",
            .description = "second",
            .input_json_schema = "{}",
        },
        ToolResult{});

    const absl::StatusOr<ToolRegistry> registry = ToolRegistry::Create({ first, second });

    ASSERT_FALSE(registry.ok());
    EXPECT_EQ(registry.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ToolRegistryTest, CreateRejectsNullTool) {
    const absl::StatusOr<ToolRegistry> registry =
        ToolRegistry::Create({ std::shared_ptr<const Tool>{} });

    ASSERT_FALSE(registry.ok());
    EXPECT_EQ(registry.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ToolRegistryTest, ExecuteNormalizesMissingResultMetadata) {
    const auto tool = std::make_shared<StaticTool>(
        ToolDefinition{
            .name = "echo_tool",
            .description = "echo",
            .input_json_schema = "{}",
        },
        ToolResult{
            .output_text = "ok",
        });
    const absl::StatusOr<ToolRegistry> registry = ToolRegistry::Create({ tool });
    ASSERT_TRUE(registry.ok()) << registry.status();

    const absl::StatusOr<ToolResult> result =
        registry->Execute(ToolExecutionContext{}, ToolCall{
                                                      .call_id = "call_1",
                                                      .name = "echo_tool",
                                                      .arguments_json = "{}",
                                                  });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->call_id, "call_1");
    EXPECT_EQ(result->tool_name, "echo_tool");
    EXPECT_EQ(result->output_text, "ok");
    EXPECT_FALSE(result->is_error);
}

TEST(ToolRegistryTest, ExecuteReturnsNotFoundForUnknownTool) {
    const auto tool = std::make_shared<StaticTool>(
        ToolDefinition{
            .name = "echo_tool",
            .description = "echo",
            .input_json_schema = "{}",
        },
        ToolResult{
            .output_text = "ok",
        });
    const absl::StatusOr<ToolRegistry> registry = ToolRegistry::Create({ tool });
    ASSERT_TRUE(registry.ok()) << registry.status();

    const absl::StatusOr<ToolResult> result =
        registry->Execute(ToolExecutionContext{}, ToolCall{
                                                      .call_id = "call_2",
                                                      .name = "missing_tool",
                                                      .arguments_json = "{}",
                                                  });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace isla::server::tools
