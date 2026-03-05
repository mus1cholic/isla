#include "win32_layered_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include "absl/strings/match.h"
#include "shared/src/test_runfiles.hpp"
#include <gtest/gtest.h>

namespace isla::client {

namespace {

std::string read_text_file(const std::string& file_path) {
    std::ifstream stream(file_path, std::ios::binary);
    if (!stream.is_open()) {
        return "";
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string load_client_source() {
    const std::string path =
        isla::shared::test::runfile_path("client/src/win32_layered_overlay.cpp");
    return read_text_file(path);
}

std::string normalize_source(std::string_view source) {
    std::string result(source);
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }),
                 result.end());
    return result;
}

bool normalized_source_contains(std::string_view normalized_source, std::string_view needle) {
    return absl::StrContains(normalized_source, normalize_source(needle));
}

} // namespace

TEST(Win32LayeredOverlayTest, ConfigureReturnsFalseForNullWindow) {
    EXPECT_FALSE(configure_win32_alpha_composited_overlay(nullptr));
}

TEST(Win32LayeredOverlayTest, PrefersNonLayeredDirectCompositionStyleContract) {
    const std::string source = load_client_source();
    ASSERT_FALSE(source.empty()) << "Could not load win32_layered_overlay.cpp source";
    const std::string normalized_source = normalize_source(source);

    EXPECT_TRUE(
        normalized_source_contains(normalized_source, "using non-layered DirectComposition style"));
    EXPECT_TRUE(
        normalized_source_contains(normalized_source, "~(WS_EX_LAYERED | WS_EX_TOOLWINDOW)"));
    EXPECT_TRUE(
        normalized_source_contains(normalized_source, "using layered-alpha fallback style"));
    EXPECT_TRUE(normalized_source_contains(
        normalized_source,
        "g_overlay_composition_mode = OverlayCompositionMode::LayeredFallback;"));
    EXPECT_TRUE(normalized_source_contains(
        normalized_source,
        "g_overlay_composition_mode = OverlayCompositionMode::NonLayeredDirectComposition;"));
    EXPECT_TRUE(normalized_source_contains(
        normalized_source, "skipping DwmEnableBlurBehindWindow/DwmExtendFrameIntoClientArea"));
    EXPECT_FALSE(normalized_source_contains(normalized_source, "DwmEnableBlurBehindWindow("));
    EXPECT_FALSE(normalized_source_contains(normalized_source, "DwmExtendFrameIntoClientArea("));
}

TEST(Win32LayeredOverlayTest, RefreshReappliesLayeredAlphaInFallbackModeContract) {
    const std::string source = load_client_source();
    ASSERT_FALSE(source.empty()) << "Could not load win32_layered_overlay.cpp source";
    const std::string normalized_source = normalize_source(source);

    EXPECT_TRUE(normalized_source_contains(
        normalized_source,
        "g_overlay_composition_mode == OverlayCompositionMode::LayeredFallback"));
    EXPECT_TRUE(normalized_source_contains(normalized_source,
                                           "SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA)"));
    EXPECT_TRUE(normalized_source_contains(normalized_source, "layered fallback refresh failed"));
}

} // namespace isla::client
