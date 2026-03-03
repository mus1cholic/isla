#include "engine/src/render/include/model_renderer_skinning_utils.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace isla::client {
namespace {

TEST(ModelRendererSkinningUtilsTest, ProgramSelectionFallsBackToStaticWhenAnyGateFails) {
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = false,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = false,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = false,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = false,
              }),
              SkinningProgramPath::StaticMesh);
}

TEST(ModelRendererSkinningUtilsTest, ProgramSelectionUsesSkinnedWhenAllGatesPass) {
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::SkinnedMesh);
}

TEST(ModelRendererSkinningUtilsTest, FillSkinPaletteBufferCopiesAndPadsWithIdentity) {
    std::vector<Mat4> upload(4U, Mat4::translation(Vec3{ .x = 99.0F, .y = 99.0F, .z = 99.0F }));
    const std::vector<Mat4> source = {
        Mat4::translation(Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F }),
        Mat4::translation(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F }),
    };

    std::size_t copied = 0U;
    const bool truncated = fill_skin_palette_upload_buffer(source, upload, &copied);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(copied, 2U);
    EXPECT_NEAR(upload[0].elements[12], 1.0F, 1.0e-6F);
    EXPECT_NEAR(upload[1].elements[12], 2.0F, 1.0e-6F);
    EXPECT_NEAR(upload[2].elements[12], 0.0F, 1.0e-6F);
    EXPECT_NEAR(upload[3].elements[12], 0.0F, 1.0e-6F);
}

TEST(ModelRendererSkinningUtilsTest, FillSkinPaletteBufferReportsTruncationAtLimitBoundary) {
    std::vector<Mat4> source(kMaxGpuSkinningJoints + 1U, Mat4::identity());
    source[kMaxGpuSkinningJoints - 1U] =
        Mat4::translation(Vec3{ .x = 64.0F, .y = 0.0F, .z = 0.0F });
    source[kMaxGpuSkinningJoints] =
        Mat4::translation(Vec3{ .x = 65.0F, .y = 0.0F, .z = 0.0F });
    std::vector<Mat4> upload(kMaxGpuSkinningJoints, Mat4::identity());

    std::size_t copied = 0U;
    const bool truncated = fill_skin_palette_upload_buffer(source, upload, &copied);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(copied, kMaxGpuSkinningJoints);
    EXPECT_NEAR(upload[kMaxGpuSkinningJoints - 1U].elements[12], 64.0F, 1.0e-6F);
}

} // namespace
} // namespace isla::client
