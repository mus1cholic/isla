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

TEST(WindowsCompositionContractTest, ModelRendererUsesDirectCompositionSwapchainContract) {
    const std::string source = load_source_file("engine/src/render/model_renderer.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load model_renderer.cpp source";

    EXPECT_TRUE(contains_normalized(source, "CreateSwapChainForComposition("));
    EXPECT_TRUE(contains_normalized(source, "DXGI_ALPHA_MODE_PREMULTIPLIED"));
    EXPECT_TRUE(contains_normalized(source, "platform_data.nwh = nullptr;"));
    EXPECT_TRUE(contains_normalized(source, "platform_data.context = presenter.device.Get();"));
    EXPECT_TRUE(contains_normalized(
        source, "platform_data.backBuffer = presenter.render_target_view.Get();"));
    EXPECT_TRUE(contains_normalized(source, "bgfx::setPlatformData(platform_data);"));
}

TEST(WindowsCompositionContractTest, ModelRendererResizeRebindsExternalBackbufferContract) {
    const std::string source = load_source_file("engine/src/render/model_renderer.cpp");
    ASSERT_FALSE(source.empty()) << "Could not load model_renderer.cpp source";

    EXPECT_TRUE(contains_normalized(source, "resize_direct_composition_presenter("));
    EXPECT_TRUE(contains_normalized(source, "impl_->window_width == impl_->presenter.width"));
    EXPECT_TRUE(contains_normalized(source, "bgfx::setPlatformData(platform_data);"));
    EXPECT_TRUE(contains_normalized(
        source, "bgfx::reset(static_cast<std::uint32_t>(impl_->window_width),"));
    EXPECT_TRUE(contains_normalized(source, "DirectComposition presenter resize failed; keeping"));
}

} // namespace
