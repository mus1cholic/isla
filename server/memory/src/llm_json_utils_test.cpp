#include "isla/server/memory/llm_json_utils.hpp"

#include <gtest/gtest.h>

namespace isla::server::memory {
namespace {

TEST(StripMarkdownCodeFencesTest, PlainJsonPassesThrough) {
    const std::string input = R"({"should_flush": false})";
    EXPECT_EQ(StripMarkdownCodeFences(input), input);
}

TEST(StripMarkdownCodeFencesTest, FencedJsonWithLanguageTag) {
    const std::string input = "```json\n{\"should_flush\": false}\n```";
    EXPECT_EQ(StripMarkdownCodeFences(input), R"({"should_flush": false})");
}

TEST(StripMarkdownCodeFencesTest, FencedJsonWithoutLanguageTag) {
    const std::string input = "```\n{\"should_flush\": true}\n```";
    EXPECT_EQ(StripMarkdownCodeFences(input), R"({"should_flush": true})");
}

TEST(StripMarkdownCodeFencesTest, SingleLineFenced) {
    const std::string input = "```{\"should_flush\": false}```";
    EXPECT_EQ(StripMarkdownCodeFences(input), R"({"should_flush": false})");
}

TEST(StripMarkdownCodeFencesTest, SurroundingWhitespace) {
    const std::string input = "  \n```json\n{\"key\": \"val\"}\n```\n  ";
    EXPECT_EQ(StripMarkdownCodeFences(input), R"({"key": "val"})");
}

TEST(StripMarkdownCodeFencesTest, MultilineJsonBody) {
    const std::string input = "```json\n"
                              "{\n"
                              "  \"tier1_detail\": \"hello\",\n"
                              "  \"tier2_summary\": \"world\"\n"
                              "}\n"
                              "```";
    const std::string expected = "{\n"
                                 "  \"tier1_detail\": \"hello\",\n"
                                 "  \"tier2_summary\": \"world\"\n"
                                 "}";
    EXPECT_EQ(StripMarkdownCodeFences(input), expected);
}

TEST(StripMarkdownCodeFencesTest, OnlyOpeningFenceReturnsUnchanged) {
    const std::string input = "```json\n{\"key\": \"val\"}";
    EXPECT_EQ(StripMarkdownCodeFences(input), input);
}

TEST(StripMarkdownCodeFencesTest, OnlyClosingFenceReturnsUnchanged) {
    const std::string input = "{\"key\": \"val\"}\n```";
    EXPECT_EQ(StripMarkdownCodeFences(input), input);
}

TEST(StripMarkdownCodeFencesTest, EmptyString) {
    EXPECT_EQ(StripMarkdownCodeFences(""), "");
}

TEST(StripMarkdownCodeFencesTest, EmptyFences) {
    const std::string input = "```\n```";
    EXPECT_EQ(StripMarkdownCodeFences(input), "");
}

} // namespace
} // namespace isla::server::memory
