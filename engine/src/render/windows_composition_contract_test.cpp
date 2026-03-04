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
#include "absl/strings/string_view.h"
#include <gtest/gtest.h>

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

    const std::string needle = absl::StrCat("_main/", relative_path);
    std::string line;
    while (std::getline(manifest_stream, line)) {
        const std::vector<absl::string_view> parts = absl::StrSplit(line, absl::MaxSplits(' ', 1));
        if (parts.size() != 2U) {
            continue;
        }
        if (parts.at(0) == needle) {
            return std::string(parts.at(1));
        }
    }

    return "";
}

std::string load_source_file(std::string_view relative_path_view) {
    const std::string relative_path(relative_path_view);
    std::vector<std::filesystem::path> candidates{
        std::filesystem::path(relative_path_view),
        std::filesystem::path("..") / relative_path_view,
    };

    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (!c_string_is_null_or_empty(test_srcdir)) {
        candidates.emplace_back(std::filesystem::path(test_srcdir) / "_main" / relative_path);
    }

    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    if (!c_string_is_null_or_empty(runfiles_dir)) {
        candidates.emplace_back(std::filesystem::path(runfiles_dir) / "_main" / relative_path);
    }

    const std::string manifest_resolved_path = find_path_in_runfiles_manifest(relative_path);
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

TEST(WindowsCompositionContractTest, ModelRendererUsesDirectCompositionSwapchainContract) {
    const std::string source = load_source_file("engine/src/render/model_renderer.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load model_renderer.cpp source";

    EXPECT_TRUE(absl::StrContains(source, "CreateSwapChainForComposition"));
    EXPECT_TRUE(absl::StrContains(source, "DXGI_ALPHA_MODE_PREMULTIPLIED"));
    EXPECT_TRUE(absl::StrContains(source, "platform_data.nwh = nullptr;"));
    EXPECT_TRUE(absl::StrContains(source, "platform_data.context = presenter.device.Get();"));
    EXPECT_TRUE(absl::StrContains(
        source, "platform_data.backBuffer = presenter.render_target_view.Get();"));
    EXPECT_TRUE(absl::StrContains(source, "bgfx::setPlatformData(platform_data);"));
}

TEST(WindowsCompositionContractTest, ModelRendererResizeRebindsExternalBackbufferContract) {
    const std::string source = load_source_file("engine/src/render/model_renderer.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load model_renderer.cpp source";

    EXPECT_TRUE(absl::StrContains(source, "resize_direct_composition_presenter("));
    EXPECT_TRUE(absl::StrContains(source, "impl_->window_width == impl_->presenter.width"));
    EXPECT_TRUE(absl::StrContains(source, "bgfx::setPlatformData(platform_data);"));
    EXPECT_TRUE(absl::StrContains(source, "bgfx::reset(static_cast<std::uint32_t>(impl_->window_width),"));
    EXPECT_TRUE(absl::StrContains(source, "DirectComposition presenter resize failed; keeping "));
}

TEST(WindowsCompositionContractTest, OverlayPrefersNonLayeredDirectCompositionStyleContract) {
    const std::string source = load_source_file("client/src/win32_layered_overlay.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load win32_layered_overlay.cpp source";

    EXPECT_TRUE(absl::StrContains(source, "using non-layered DirectComposition style"));
    EXPECT_TRUE(absl::StrContains(source, "~(WS_EX_LAYERED | WS_EX_TOOLWINDOW)"));
    EXPECT_TRUE(absl::StrContains(source, "using layered-alpha fallback style"));
    EXPECT_TRUE(absl::StrContains(
        source, "skipping DwmEnableBlurBehindWindow/DwmExtendFrameIntoClientArea"));
    EXPECT_FALSE(absl::StrContains(source, "DwmEnableBlurBehindWindow("));
    EXPECT_FALSE(absl::StrContains(source, "DwmExtendFrameIntoClientArea("));
}

} // namespace
