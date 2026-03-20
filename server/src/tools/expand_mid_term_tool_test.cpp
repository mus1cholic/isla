#include "isla/server/tools/expand_mid_term_tool.hpp"

#include <gtest/gtest.h>

namespace isla::server::tools {
namespace {

TEST(ExpandMidTermToolTest, DefinitionPublishesExpectedPublicContract) {
    const ExpandMidTermTool tool;

    const ToolDefinition definition = tool.Definition();

    EXPECT_EQ(definition.name, "expand_mid_term");
    EXPECT_TRUE(definition.read_only);
    EXPECT_NE(definition.description.find("[expandable]"), std::string::npos);
    EXPECT_NE(definition.input_json_schema.find("\"episode_id\""), std::string::npos);
    EXPECT_NE(definition.input_json_schema.find("\"required\": [\"episode_id\"]"),
              std::string::npos);
    EXPECT_NE(definition.input_json_schema.find("\"additionalProperties\": false"),
              std::string::npos);
}

TEST(ExpandMidTermToolTest, ExecuteRejectsMissingEpisodeIdAsToolError) {
    const ExpandMidTermTool tool;

    const absl::StatusOr<ToolResult> result =
        tool.Execute(ToolExecutionContext{}, ToolCall{
                                                 .call_id = "call_1",
                                                 .name = "expand_mid_term",
                                                 .arguments_json = "{}",
                                             });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->is_error);
    EXPECT_EQ(result->tool_name, "expand_mid_term");
    EXPECT_NE(result->output_text.find("episode_id"), std::string::npos);
}

TEST(ExpandMidTermToolTest, ExecuteRejectsMalformedJsonAsToolError) {
    const ExpandMidTermTool tool;

    const absl::StatusOr<ToolResult> result =
        tool.Execute(ToolExecutionContext{}, ToolCall{
                                                 .call_id = "call_bad_json",
                                                 .name = "expand_mid_term",
                                                 .arguments_json = "{not json",
                                             });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->is_error);
    EXPECT_EQ(result->tool_name, "expand_mid_term");
    EXPECT_NE(result->output_text.find("episode_id"), std::string::npos);
}

TEST(ExpandMidTermToolTest, ExecuteRejectsUnexpectedExtraFieldsAsToolError) {
    const ExpandMidTermTool tool;

    const absl::StatusOr<ToolResult> result =
        tool.Execute(ToolExecutionContext{},
                     ToolCall{
                         .call_id = "call_extra",
                         .name = "expand_mid_term",
                         .arguments_json = R"json({"episode_id":"ep_123","extra":"nope"})json",
                     });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->is_error);
    EXPECT_EQ(result->tool_name, "expand_mid_term");
    EXPECT_NE(result->output_text.find("accepts only one argument"), std::string::npos);
}

TEST(ExpandMidTermToolTest, ExecuteRejectsMismatchedToolNameWithFrameworkError) {
    const ExpandMidTermTool tool;

    const absl::StatusOr<ToolResult> result = tool.Execute(
        ToolExecutionContext{}, ToolCall{
                                    .call_id = "call_wrong_name",
                                    .name = "other_tool",
                                    .arguments_json = R"json({"episode_id":"ep_123"})json",
                                });

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ExpandMidTermToolTest, ExecuteReturnsPlaceholderUntilWired) {
    const ExpandMidTermTool tool;

    const absl::StatusOr<ToolResult> result = tool.Execute(
        ToolExecutionContext{}, ToolCall{
                                    .call_id = "call_2",
                                    .name = "expand_mid_term",
                                    .arguments_json = R"json({"episode_id":"ep_123"})json",
                                });

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->is_error);
    EXPECT_EQ(result->call_id, "call_2");
    EXPECT_EQ(result->tool_name, "expand_mid_term");
    EXPECT_NE(result->output_text.find("not yet wired"), std::string::npos);
}

} // namespace
} // namespace isla::server::tools
