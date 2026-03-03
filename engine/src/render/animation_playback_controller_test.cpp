#include "engine/src/render/include/animation_playback_controller.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <utility>

namespace isla::client::animated_gltf {
namespace {

AnimatedGltfAsset make_single_joint_asset() {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(1U);
    asset.skeleton.joints[0].parent_index = -1;
    asset.bind_local_transforms.resize(1U);
    asset.bind_prefix_matrices = { Mat4::identity() };

    AnimationClip clip;
    clip.name = "idle";
    clip.duration_seconds = 1.0F;
    clip.joint_tracks.resize(1U);
    clip.joint_tracks[0].translations = {
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };

    asset.clips.push_back(std::move(clip));
    return asset;
}

float skin_tx(const EvaluatedPose& pose) {
    return pose.skin_matrices.at(0U).elements[12];
}

TEST(AnimationPlaybackControllerTest, RejectsAssetWithNoClips) {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(1U);
    asset.bind_local_transforms.resize(1U);
    AnimationPlaybackController controller;
    std::string error;
    EXPECT_FALSE(controller.set_asset(&asset, &error));
    EXPECT_EQ(error, "animation asset has no clips");
}

TEST(AnimationPlaybackControllerTest, EvaluatesInitialPoseOnSetAsset) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    ASSERT_TRUE(controller.has_cached_pose());
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 0.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, TickAdvancesBySpeedWhenPlaying) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.set_speed(2.0F);
    ASSERT_TRUE(controller.tick(0.25F, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.5F, 1.0e-4F);
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 1.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, TickDoesNotAdvanceWhenPaused) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.set_playing(false);
    ASSERT_TRUE(controller.tick(0.5F, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.0F, 1.0e-4F);
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 0.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, ClampModeSticksAtClipEnd) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.set_playback_mode(ClipPlaybackMode::Clamp);
    ASSERT_TRUE(controller.seek_local_time(1.25F, nullptr));
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 2.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, LoopModeWrapsAtClipEnd) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.set_playback_mode(ClipPlaybackMode::Loop);
    ASSERT_TRUE(controller.seek_local_time(1.0F, nullptr));
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 0.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, InvalidSpeedFallsBackToOne) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.set_speed(-2.0F);
    ASSERT_TRUE(controller.tick(0.5F, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.5F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, SetClipIndexResetsLocalTimeToZero) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationClip second_clip;
    second_clip.name = "walk";
    second_clip.duration_seconds = 1.0F;
    second_clip.joint_tracks.resize(1U);
    second_clip.joint_tracks[0].translations = {
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } },
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 3.0F, .y = 0.0F, .z = 0.0F } },
    };
    asset.clips.push_back(std::move(second_clip));

    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    ASSERT_TRUE(controller.seek_local_time(0.75F, nullptr));
    ASSERT_TRUE(controller.set_clip_index(1U, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.0F, 1.0e-4F);
    EXPECT_NEAR(skin_tx(controller.cached_pose()), 1.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, SeekAndTickSanitizeInvalidTimes) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));

    ASSERT_TRUE(controller.seek_local_time(-5.0F, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.0F, 1.0e-4F);

    ASSERT_TRUE(controller.seek_local_time(std::numeric_limits<float>::infinity(), nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.0F, 1.0e-4F);

    ASSERT_TRUE(controller.tick(-2.0F, nullptr));
    EXPECT_NEAR(controller.state().local_time_seconds, 0.0F, 1.0e-4F);
}

TEST(AnimationPlaybackControllerTest, TickFailsWhenAssetCleared) {
    AnimatedGltfAsset asset = make_single_joint_asset();
    AnimationPlaybackController controller;
    ASSERT_TRUE(controller.set_asset(&asset, nullptr));
    controller.clear_asset();

    std::string error;
    EXPECT_FALSE(controller.tick(0.016F, &error));
    EXPECT_EQ(error, "animation asset is not set");
}

} // namespace
} // namespace isla::client::animated_gltf
