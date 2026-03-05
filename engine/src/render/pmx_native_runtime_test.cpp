#include "isla/engine/render/pmx_native_runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "shared/src/test_runfiles.hpp"

namespace isla::client::pmx_native {
namespace {

bool contains_message_with_substring(const std::vector<std::string>& messages,
                                     std::string_view needle) {
    return std::any_of(messages.begin(), messages.end(), [needle](const std::string& message) {
        return message.find(needle) != std::string::npos;
    });
}

class PmxNativeRuntimeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() /
                    std::filesystem::path("isla_pmx_native_runtime_test");
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
        std::filesystem::create_directories(temp_dir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_path(const char* filename) const {
        return temp_dir_ / filename;
    }

    std::filesystem::path temp_dir_;
};

TEST_F(PmxNativeRuntimeTest, SupportMatrixDocumentsPhaseZeroBoundary) {
    const SupportMatrix& matrix = support_matrix();

    EXPECT_EQ(matrix.parse_and_diagnostics_boundary, SupportStatus::Supported);
    EXPECT_EQ(matrix.runtime_mesh_skeleton_material_bridge, SupportStatus::Planned);
    EXPECT_EQ(matrix.bind_pose_display, SupportStatus::Planned);
    EXPECT_EQ(matrix.skinning_bdef1, SupportStatus::Planned);
    EXPECT_EQ(matrix.skinning_bdef2, SupportStatus::Planned);
    EXPECT_EQ(matrix.skinning_bdef4, SupportStatus::Planned);
    EXPECT_EQ(matrix.skinning_sdef, SupportStatus::ExplicitFallback);
    EXPECT_EQ(matrix.skinning_qdef, SupportStatus::ExplicitFallback);
    EXPECT_EQ(matrix.baseline_albedo_alpha_cull_mapping, SupportStatus::Planned);
    EXPECT_EQ(matrix.toon_sphere_edge_material_channels, SupportStatus::Deferred);
    EXPECT_EQ(matrix.morph_runtime_parity, SupportStatus::Deferred);
    EXPECT_EQ(matrix.vmd_playback, SupportStatus::Deferred);
    EXPECT_EQ(matrix.pmx_physics, SupportStatus::Deferred);
    EXPECT_EQ(backend_name(), "saba");
    EXPECT_EQ(backend_version_pin(), "29b8efa8b31c8e746f9a88020fb0ad9dcdcf3332");
}

TEST_F(PmxNativeRuntimeTest, IngestionPolicyDocumentsCoordinateAndPathRules) {
    const IngestionPolicy& policy = ingestion_policy();

    EXPECT_TRUE(policy.normalize_names_to_utf8);
    EXPECT_TRUE(policy.normalize_texture_paths_to_asset_directory);
    EXPECT_TRUE(policy.reject_absolute_texture_paths);
    EXPECT_TRUE(policy.reject_parent_traversal_texture_paths);
    EXPECT_TRUE(policy.mirror_z_axis_into_isla_left_handed_space);
    EXPECT_TRUE(policy.flip_v_texture_coordinate);
}

TEST_F(PmxNativeRuntimeTest, ProbeRejectsNonPmxExtension) {
    const std::filesystem::path asset_path = temp_path("model.txt");
    std::ofstream(asset_path) << "not a pmx";

    const ProbeResult result = probe_file(asset_path.string());

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error_message.find(".pmx"), std::string::npos);
}

TEST_F(PmxNativeRuntimeTest, ProbeSurfacesParseFailureForInvalidPmxData) {
    const std::filesystem::path asset_path = temp_path("invalid_model.pmx");
    std::ofstream(asset_path, std::ios::binary) << "not_a_real_pmx";

    const ProbeResult result = probe_file(asset_path.string());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_NE(result.error_message.find("backend='saba'"), std::string::npos);
    EXPECT_NE(result.error_message.find(asset_path.string()), std::string::npos);
}

TEST_F(PmxNativeRuntimeTest, ProbeSummarizesMinimalValidFixture) {
    const ProbeResult result = probe_file(isla::shared::test::runfile_path(
        "engine/src/render/testdata/pmx_phase0_valid_minimal.pmx"));

    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(result.summary.model_name, "phase0_valid");
    EXPECT_EQ(result.summary.vertex_count, 3U);
    EXPECT_EQ(result.summary.face_count, 1U);
    EXPECT_EQ(result.summary.material_count, 1U);
    EXPECT_EQ(result.summary.texture_count, 0U);
    EXPECT_EQ(result.summary.bone_count, 1U);
    EXPECT_EQ(result.summary.morph_count, 0U);
    EXPECT_EQ(result.summary.rigidbody_count, 0U);
    EXPECT_EQ(result.summary.joint_count, 0U);
    EXPECT_EQ(result.summary.softbody_count, 0U);
    EXPECT_EQ(result.summary.ik_bone_count, 0U);
    EXPECT_EQ(result.summary.append_transform_bone_count, 0U);
    EXPECT_EQ(result.summary.bdef1_vertex_count, 3U);
    EXPECT_EQ(result.summary.bdef2_vertex_count, 0U);
    EXPECT_EQ(result.summary.bdef4_vertex_count, 0U);
    EXPECT_EQ(result.summary.sdef_vertex_count, 0U);
    EXPECT_EQ(result.summary.qdef_vertex_count, 0U);
    EXPECT_EQ(result.summary.absolute_texture_reference_count, 0U);
    EXPECT_EQ(result.summary.parent_traversal_texture_reference_count, 0U);
    EXPECT_EQ(result.summary.missing_texture_reference_count, 0U);
    EXPECT_EQ(result.summary.sphere_texture_material_count, 0U);
    EXPECT_EQ(result.summary.toon_texture_material_count, 0U);
    EXPECT_EQ(result.summary.edge_enabled_material_count, 0U);
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_TRUE(contains_message_with_substring(result.infos, "asset='"));
    EXPECT_TRUE(contains_message_with_substring(result.infos, "warnings=0"));
    EXPECT_TRUE(contains_message_with_substring(result.infos, "skinning={BDEF1:3"));
    EXPECT_TRUE(contains_message_with_substring(result.infos, "PMX probe succeeded"));
}

TEST_F(PmxNativeRuntimeTest, ProbeSurfacesDeferredAndFallbackWarningsFromFixture) {
    const ProbeResult result = probe_file(isla::shared::test::runfile_path(
        "engine/src/render/testdata/pmx_phase0_feature_warnings.pmx"));

    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(result.summary.model_name, "phase0_feature_warning");
    EXPECT_EQ(result.summary.vertex_count, 5U);
    EXPECT_EQ(result.summary.face_count, 1U);
    EXPECT_EQ(result.summary.material_count, 1U);
    EXPECT_EQ(result.summary.texture_count, 4U);
    EXPECT_EQ(result.summary.bone_count, 2U);
    EXPECT_EQ(result.summary.morph_count, 1U);
    EXPECT_EQ(result.summary.rigidbody_count, 1U);
    EXPECT_EQ(result.summary.joint_count, 1U);
    EXPECT_EQ(result.summary.softbody_count, 0U);
    EXPECT_EQ(result.summary.ik_bone_count, 1U);
    EXPECT_EQ(result.summary.append_transform_bone_count, 1U);
    EXPECT_EQ(result.summary.bdef1_vertex_count, 1U);
    EXPECT_EQ(result.summary.bdef2_vertex_count, 1U);
    EXPECT_EQ(result.summary.bdef4_vertex_count, 1U);
    EXPECT_EQ(result.summary.sdef_vertex_count, 1U);
    EXPECT_EQ(result.summary.qdef_vertex_count, 1U);
    EXPECT_EQ(result.summary.absolute_texture_reference_count, 1U);
    EXPECT_EQ(result.summary.parent_traversal_texture_reference_count, 1U);
    EXPECT_EQ(result.summary.missing_texture_reference_count, 2U);
    EXPECT_EQ(result.summary.sphere_texture_material_count, 1U);
    EXPECT_EQ(result.summary.toon_texture_material_count, 1U);
    EXPECT_EQ(result.summary.edge_enabled_material_count, 1U);

    EXPECT_TRUE(contains_message_with_substring(result.warnings, "SDEF vertices count=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "QDEF vertices count=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "morph data count=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "rigidbodies=1 joints=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "sphere_materials=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "absolute_texture_refs=1"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "count=2"));
    EXPECT_TRUE(contains_message_with_substring(result.warnings, "missing asset-relative textures"));
    EXPECT_TRUE(contains_message_with_substring(result.infos, "warnings=7"));
}

TEST_F(PmxNativeRuntimeTest, SupportStatusNamesRemainStable) {
    EXPECT_STREQ(support_status_name(SupportStatus::Supported), "supported");
    EXPECT_STREQ(support_status_name(SupportStatus::Planned), "planned");
    EXPECT_STREQ(support_status_name(SupportStatus::ExplicitFallback), "explicit_fallback");
    EXPECT_STREQ(support_status_name(SupportStatus::Deferred), "deferred");
    EXPECT_STREQ(skinning_mode_name(SkinningMode::Bdef1), "BDEF1");
    EXPECT_STREQ(skinning_mode_name(SkinningMode::Sdef), "SDEF");
    EXPECT_STREQ(skinning_mode_name(SkinningMode::Qdef), "QDEF");
}

} // namespace
} // namespace isla::client::pmx_native
