#include "ai_gateway_logging_utils.hpp"

#include <string_view>

#include <gtest/gtest.h>

namespace isla::server::ai_gateway {
namespace {

TEST(AiGatewayLoggingUtilsTest, LeavesPrintableTextUnchanged) {
    EXPECT_EQ(SanitizeForLog("turn_123"), "turn_123");
}

TEST(AiGatewayLoggingUtilsTest, EscapesCommonControlCharacters) {
    EXPECT_EQ(SanitizeForLog("line1\nline2\r\tend"), "line1\\nline2\\r\\tend");
}

TEST(AiGatewayLoggingUtilsTest, EscapesOtherControlBytesAsHex) {
    EXPECT_EQ(SanitizeForLog(std::string_view("a\x01\x7f", 3)), "a\\x01\\x7F");
}

} // namespace
} // namespace isla::server::ai_gateway
