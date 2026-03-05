#include "isla/engine/render/pmx_texture_remap_sidecar.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace isla::client::pmx_texture_remap_sidecar {
namespace {

class ScopedTempDir {
  public:
    static ScopedTempDir create(std::string_view prefix) {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return {};
        }
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<std::uint64_t> distribution;
        for (int i = 0; i < 100; ++i) {
            const auto candidate =
                base / (std::string(prefix) + "_" + std::to_string(distribution(rng)));
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                return ScopedTempDir(candidate);
            }
            ec.clear();
        }
        return {};
    }

    ScopedTempDir() = default;
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = default;
    ScopedTempDir& operator=(ScopedTempDir&&) = default;

    ~ScopedTempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] bool is_valid() const {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    explicit ScopedTempDir(std::filesystem::path path) : path_(std::move(path)) {}
    std::filesystem::path path_{};
};

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << text;
    ASSERT_TRUE(out.good());
}

TEST(PmxTextureRemapSidecarTest, LoadsValidMaterialNameMapping) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    std::filesystem::create_directories(temp_dir.path() / "textures");
    write_text_file(temp_dir.path() / "textures" / "head.png", "fake");
    write_text_file(sidecar_path, "{"
                                  "\"schema_version\":\"1.0.0\","
                                  "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":"
                                  "\"asset_relative_only\"},"
                                  "\"mappings\":[{"
                                  "\"id\":\"head\","
                                  "\"target\":{\"material_name\":\"Head\"},"
                                  "\"albedo_texture\":\"textures/head.png\","
                                  "\"alpha_cutoff\":0.5"
                                  "}]"
                                  "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.mappings.size(), 1U);
    EXPECT_EQ(loaded.sidecar.override_mode, OverrideMode::IfMissing);
    EXPECT_EQ(loaded.sidecar.mappings[0].target.material_name, std::optional<std::string>("Head"));
    EXPECT_EQ(loaded.sidecar.mappings[0].albedo_texture_path,
              (temp_dir.path() / "textures" / "head.png").lexically_normal().string());
    ASSERT_TRUE(loaded.sidecar.mappings[0].alpha_cutoff.has_value());
    EXPECT_NEAR(*loaded.sidecar.mappings[0].alpha_cutoff, 0.5F, 1.0e-6F);
}

TEST(PmxTextureRemapSidecarTest, FailsWhenOverrideModeInvalid) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(sidecar_path, "{"
                                  "\"schema_version\":\"1.0.0\","
                                  "\"policy\":{\"override_mode\":\"sometimes\",\"path_scope\":"
                                  "\"asset_relative_only\"},"
                                  "\"mappings\":[]"
                                  "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("override_mode"), std::string::npos);
}

TEST(PmxTextureRemapSidecarTest, FailsWhenPathScopeInvalid) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(sidecar_path,
                    "{"
                    "\"schema_version\":\"1.0.0\","
                    "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":\"anywhere\"},"
                    "\"mappings\":[]"
                    "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("path_scope"), std::string::npos);
}

TEST(PmxTextureRemapSidecarTest, FailsWhenTargetUsesBothKeyModes) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(
        sidecar_path,
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":"
        "\"asset_relative_only\"},"
        "\"mappings\":[{"
        "\"id\":\"bad\","
        "\"target\":{\"material_name\":\"Head\",\"mesh_index\":0,\"primitive_index\":0},"
        "\"albedo_texture\":\"head.png\""
        "}]"
        "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("exactly one key mode"), std::string::npos);
}

TEST(PmxTextureRemapSidecarTest, FailsWhenAlphaCutoffOutOfRange) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(sidecar_path, "{"
                                  "\"schema_version\":\"1.0.0\","
                                  "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":"
                                  "\"asset_relative_only\"},"
                                  "\"mappings\":[{"
                                  "\"id\":\"bad_alpha\","
                                  "\"target\":{\"material_name\":\"Head\"},"
                                  "\"albedo_texture\":\"head.png\","
                                  "\"alpha_cutoff\":1.5"
                                  "}]"
                                  "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("alpha_cutoff"), std::string::npos);
}

TEST(PmxTextureRemapSidecarTest, WarnsWhenTexturePathRejectedByHardening) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(sidecar_path, "{"
                                  "\"schema_version\":\"1.0.0\","
                                  "\"policy\":{\"override_mode\":\"always\",\"path_scope\":"
                                  "\"asset_relative_only\"},"
                                  "\"mappings\":[{"
                                  "\"id\":\"rejected\","
                                  "\"target\":{\"material_name\":\"Head\"},"
                                  "\"albedo_texture\":\"../outside.png\""
                                  "}]"
                                  "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.mappings.size(), 1U);
    EXPECT_TRUE(loaded.sidecar.mappings[0].albedo_texture_path.empty());
    EXPECT_FALSE(loaded.warnings.empty());
}

TEST(PmxTextureRemapSidecarTest, FailsWhenTargetIndexUsesFloatingPointNumber) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(
        sidecar_path,
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"policy\":{\"override_mode\":\"always\",\"path_scope\":\"asset_relative_only\"},"
        "\"mappings\":[{"
        "\"id\":\"float_index\","
        "\"target\":{\"mesh_index\":0.0,\"primitive_index\":1},"
        "\"albedo_texture\":\"head.png\""
        "}]"
        "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("mesh_index is invalid"), std::string::npos);
}

TEST(PmxTextureRemapSidecarTest, AcceptsLargeExactIntegerTargetIndices) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_texturemap_sidecar_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path asset_path = temp_dir.path() / "model.gltf";
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.texturemap.json";
    write_text_file(asset_path, "{}");
    write_text_file(
        sidecar_path,
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"policy\":{\"override_mode\":\"always\",\"path_scope\":\"asset_relative_only\"},"
        "\"mappings\":[{"
        "\"id\":\"large_index\","
        "\"target\":{\"mesh_index\":9007199254740993,\"primitive_index\":1},"
        "\"albedo_texture\":\"head.png\""
        "}]"
        "}");

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), asset_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.mappings.size(), 1U);
    ASSERT_TRUE(loaded.sidecar.mappings[0].target.mesh_index.has_value());
    EXPECT_EQ(*loaded.sidecar.mappings[0].target.mesh_index, 9007199254740993ULL);
}

} // namespace
} // namespace isla::client::pmx_texture_remap_sidecar
