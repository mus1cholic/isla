#include <gtest/gtest.h>

#include <string>

#include "engine/src/render/include/animated_gltf.hpp"
#include "shared/src/test_runfiles.hpp"

namespace isla::client::animated_gltf {

TEST(AnimatedGltfLoaderTests, RejectsNonSkinnedGltf) {
    const std::string path =
        isla::shared::test::runfile_path("engine/src/render/testdata/triangle_embedded.gltf");
    const AnimatedGltfLoadResult loaded = load_from_file(path);
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.error_message.empty());
}

TEST(AnimatedGltfPoseTests, InterpolatesSingleJointTranslation) {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.push_back(SkeletonJoint{});
    asset.bind_local_transforms.push_back(Transform{});

    AnimationClip clip;
    clip.name = "move";
    clip.duration_seconds = 1.0F;
    clip.joint_tracks.resize(1U);
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    std::string error;
    ASSERT_TRUE(evaluate_clip_pose(asset, 0U, 0.5F, pose, &error)) << error;
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 1.0F, 1.0e-4F);
}

TEST(AnimatedGltfPoseTests, EvaluatesHierarchyAndSkinMatrices) {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = -1 });
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = 0 });
    asset.bind_local_transforms.resize(2U);
    asset.bind_local_transforms[1].position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F };

    AnimationClip clip;
    clip.duration_seconds = 2.0F;
    clip.joint_tracks.resize(2U);
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 2.0F, .value = Vec3{ .x = 4.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(evaluate_clip_pose(asset, 0U, 1.0F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    ASSERT_EQ(pose.skin_matrices.size(), 2U);

    // Parent translated to x=2 at t=1. Child bind local x=1, so global child should be x=3.
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 3.0F, 1.0e-4F);
    EXPECT_NEAR(pose.skin_matrices[1].elements[12], 3.0F, 1.0e-4F);
}

} // namespace isla::client::animated_gltf

