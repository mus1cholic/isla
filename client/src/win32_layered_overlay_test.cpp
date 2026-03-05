#include "win32_layered_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include <gtest/gtest.h>

namespace isla::client {

namespace {

bool c_string_is_null_or_empty(const char* value) {
    return value == nullptr || *value == '\0';
}

std::string read_text_file(const std::filesystem::path& file_path) {
    std::ifstream stream(file_path, std::ios::binary);
    if (!stream.is_open()) {
        return "";
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string find_path_in_runfiles_manifest(const std::string& relative_path) {
    const char* manifest_path = std::getenv("RUNFILES_MANIFEST_FILE");
    if (c_string_is_null_or_empty(manifest_path)) {
        return "";
    }

    std::ifstream manifest_stream(manifest_path);
    if (!manifest_stream.is_open()) {
        return "";
    }

    const std::string needle = absl::StrCat("_main/client/src/", relative_path);
    std::string line;
    while (std::getline(manifest_stream, line)) {
        const std::vector<std::string_view> parts = absl::StrSplit(line, absl::MaxSplits(' ', 1));
        if (parts.size() != 2U) {
            continue;
        }
        if (parts.at(0) == needle) {
            return std::string(parts.at(1));
        }
    }

    return "";
}

std::string load_client_source(std::string_view file_name) {
    const std::string file(file_name);
    std::vector<std::filesystem::path> candidates{
        std::filesystem::path(file_name),
        std::filesystem::path("client/src") / file_name,
        std::filesystem::path("..") / "client/src" / file_name,
    };

    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (!c_string_is_null_or_empty(test_srcdir)) {
        candidates.emplace_back(std::filesystem::path(test_srcdir) / "_main" / "client/src" / file);
    }

    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    if (!c_string_is_null_or_empty(runfiles_dir)) {
        candidates.emplace_back(std::filesystem::path(runfiles_dir) / "_main" / "client/src" / file);
    }

    const std::string manifest_resolved_path = find_path_in_runfiles_manifest(file);
    if (!manifest_resolved_path.empty()) {
        candidates.emplace_back(manifest_resolved_path);
    }

    for (const std::filesystem::path& candidate : candidates) {
        const std::string text = read_text_file(candidate.lexically_normal());
        if (!text.empty()) {
            return text;
        }
    }

    return "";
}

std::string normalize_source(std::string_view source) {
    std::string result(source);
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }),
                 result.end());
    return result;
}

bool contains_normalized(std::string_view source, std::string_view needle) {
    return absl::StrContains(normalize_source(source), normalize_source(needle));
}

} // namespace

TEST(Win32LayeredOverlayTest, ConfigureReturnsFalseForNullWindow) {
    EXPECT_FALSE(configure_win32_alpha_composited_overlay(nullptr));
}

TEST(Win32LayeredOverlayTest, PrefersNonLayeredDirectCompositionStyleContract) {
    const std::string source = load_client_source("win32_layered_overlay.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load win32_layered_overlay.cpp source";

    EXPECT_TRUE(contains_normalized(source, "using non-layered DirectComposition style"));
    EXPECT_TRUE(contains_normalized(source, "~(WS_EX_LAYERED | WS_EX_TOOLWINDOW)"));
    EXPECT_TRUE(contains_normalized(source, "using layered-alpha fallback style"));
    EXPECT_TRUE(contains_normalized(
        source, "g_overlay_composition_mode = OverlayCompositionMode::LayeredFallback;"));
    EXPECT_TRUE(contains_normalized(
        source, "g_overlay_composition_mode = OverlayCompositionMode::NonLayeredDirectComposition;"));
    EXPECT_TRUE(contains_normalized(
        source, "skipping DwmEnableBlurBehindWindow/DwmExtendFrameIntoClientArea"));
    EXPECT_FALSE(contains_normalized(source, "DwmEnableBlurBehindWindow("));
    EXPECT_FALSE(contains_normalized(source, "DwmExtendFrameIntoClientArea("));
}

TEST(Win32LayeredOverlayTest, RefreshReappliesLayeredAlphaInFallbackModeContract) {
    const std::string source = load_client_source("win32_layered_overlay.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load win32_layered_overlay.cpp source";

    EXPECT_TRUE(contains_normalized(
        source, "g_overlay_composition_mode == OverlayCompositionMode::LayeredFallback"));
    EXPECT_TRUE(contains_normalized(source, "SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA)"));
    EXPECT_TRUE(contains_normalized(source, "layered fallback refresh failed"));
}

} // namespace isla::client
