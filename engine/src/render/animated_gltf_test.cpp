#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

TEST(AnimatedGltfLoaderTests, LoadsOnlyPrimitivesAttachedToSelectedSkin) {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() /
        ("isla_animated_gltf_" +
         std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_dir));

    const std::filesystem::path bin_path = temp_dir / "asset.bin";
    const std::filesystem::path gltf_path = temp_dir / "asset.gltf";

    std::vector<std::uint8_t> buffer;
    auto align4 = [&buffer]() {
        while ((buffer.size() % 4U) != 0U) {
            buffer.push_back(0U);
        }
    };
    auto append_f32 = [&buffer](float value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    };
    auto append_u16 = [&buffer](std::uint16_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    };

    const std::size_t unskinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(value);
    }
    const std::size_t unskinned_pos_length = buffer.size() - unskinned_pos_offset;

    const std::size_t unskinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(value);
    }
    const std::size_t unskinned_idx_length = buffer.size() - unskinned_idx_offset;

    align4();
    const std::size_t skinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(value);
    }
    const std::size_t skinned_pos_length = buffer.size() - skinned_pos_offset;

    const std::size_t skinned_joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (std::uint16_t value : { 0U, 0U, 0U, 0U }) {
            append_u16(value);
        }
    }
    const std::size_t skinned_joints_length = buffer.size() - skinned_joints_offset;

    const std::size_t skinned_weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (float value : { 1.0F, 0.0F, 0.0F, 0.0F }) {
            append_f32(value);
        }
    }
    const std::size_t skinned_weights_length = buffer.size() - skinned_weights_offset;

    const std::size_t skinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(value);
    }
    const std::size_t skinned_idx_length = buffer.size() - skinned_idx_offset;

    align4();
    const std::size_t ibm_offset = buffer.size();
    for (float value : { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 1.0F }) {
        append_f32(value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    {
        std::ofstream bin_stream(bin_path, std::ios::binary);
        ASSERT_TRUE(bin_stream.is_open());
        bin_stream.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(buffer.size()));
        ASSERT_TRUE(bin_stream.good());
    }

    {
        std::ofstream gltf_stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(gltf_stream.is_open());
        gltf_stream << "{\n"
                    << "  \"asset\": {\"version\": \"2.0\"},\n"
                    << "  \"buffers\": [{\"uri\": \"asset.bin\", \"byteLength\": " << buffer.size()
                    << "}],\n"
                    << "  \"bufferViews\": [\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << unskinned_pos_offset
                    << ", \"byteLength\": " << unskinned_pos_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << unskinned_idx_offset
                    << ", \"byteLength\": " << unskinned_idx_length << ", \"target\": 34963},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_pos_offset
                    << ", \"byteLength\": " << skinned_pos_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_joints_offset
                    << ", \"byteLength\": " << skinned_joints_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_weights_offset
                    << ", \"byteLength\": " << skinned_weights_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_idx_offset
                    << ", \"byteLength\": " << skinned_idx_length << ", \"target\": 34963},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << ibm_offset
                    << ", \"byteLength\": " << ibm_length << "}\n"
                    << "  ],\n"
                    << "  \"accessors\": [\n"
                    << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": "
                       "\"VEC3\"},\n"
                    << "    {\"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": "
                       "\"SCALAR\"},\n"
                    << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": "
                       "\"VEC3\"},\n"
                    << "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": "
                       "\"VEC4\"},\n"
                    << "    {\"bufferView\": 4, \"componentType\": 5126, \"count\": 3, \"type\": "
                       "\"VEC4\"},\n"
                    << "    {\"bufferView\": 5, \"componentType\": 5123, \"count\": 3, \"type\": "
                       "\"SCALAR\"},\n"
                    << "    {\"bufferView\": 6, \"componentType\": 5126, \"count\": 1, \"type\": "
                       "\"MAT4\"}\n"
                    << "  ],\n"
                    << "  \"meshes\": [\n"
                    << "    {\"primitives\": [{\"attributes\": {\"POSITION\": 0}, \"indices\": 1, "
                       "\"mode\": 4}]},\n"
                    << "    {\"primitives\": [{\"attributes\": {\"POSITION\": 2, \"JOINTS_0\": 3, "
                       "\"WEIGHTS_0\": 4}, \"indices\": 5, \"mode\": 4}]}\n"
                    << "  ],\n"
                    << "  \"nodes\": [\n"
                    << "    {\"name\": \"joint0\"},\n"
                    << "    {\"mesh\": 0},\n"
                    << "    {\"mesh\": 1, \"skin\": 0}\n"
                    << "  ],\n"
                    << "  \"skins\": [{\"joints\": [0], \"inverseBindMatrices\": 6}],\n"
                    << "  \"scenes\": [{\"nodes\": [1, 2]}],\n"
                    << "  \"scene\": 0\n"
                    << "}\n";
        ASSERT_TRUE(gltf_stream.good());
    }

    const AnimatedGltfLoadResult loaded = load_from_file(gltf_path.string());
    std::filesystem::remove_all(temp_dir);

    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.asset.primitives.size(), 1U);
    ASSERT_EQ(loaded.asset.primitives[0].vertices.size(), 3U);
    ASSERT_EQ(loaded.asset.primitives[0].indices.size(), 3U);
}

TEST(AnimatedGltfLoaderTests, RejectsCubicSplineInterpolation) {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() /
        ("isla_animated_gltf_" +
         std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_dir));

    const std::filesystem::path bin_path = temp_dir / "asset.bin";
    const std::filesystem::path gltf_path = temp_dir / "asset.gltf";

    std::vector<std::uint8_t> buffer;
    auto align4 = [&buffer]() {
        while ((buffer.size() % 4U) != 0U) {
            buffer.push_back(0U);
        }
    };
    auto append_f32 = [&buffer](float value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    };
    auto append_u16 = [&buffer](std::uint16_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    };

    const std::size_t skinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(value);
    }
    const std::size_t skinned_pos_length = buffer.size() - skinned_pos_offset;

    const std::size_t skinned_joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (std::uint16_t value : { 0U, 0U, 0U, 0U }) {
            append_u16(value);
        }
    }
    const std::size_t skinned_joints_length = buffer.size() - skinned_joints_offset;

    const std::size_t skinned_weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (float value : { 1.0F, 0.0F, 0.0F, 0.0F }) {
            append_f32(value);
        }
    }
    const std::size_t skinned_weights_length = buffer.size() - skinned_weights_offset;

    const std::size_t skinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(value);
    }
    const std::size_t skinned_idx_length = buffer.size() - skinned_idx_offset;

    align4();
    const std::size_t ibm_offset = buffer.size();
    for (float value : { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 1.0F }) {
        append_f32(value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    const std::size_t anim_times_offset = buffer.size();
    for (float value : { 0.0F, 1.0F }) {
        append_f32(value);
    }
    const std::size_t anim_times_length = buffer.size() - anim_times_offset;

    const std::size_t anim_translation_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F }) {
        append_f32(value);
    }
    const std::size_t anim_translation_length = buffer.size() - anim_translation_offset;

    {
        std::ofstream bin_stream(bin_path, std::ios::binary);
        ASSERT_TRUE(bin_stream.is_open());
        bin_stream.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(buffer.size()));
        ASSERT_TRUE(bin_stream.good());
    }

    {
        std::ofstream gltf_stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(gltf_stream.is_open());
        gltf_stream << "{\n"
                    << "  \"asset\": {\"version\": \"2.0\"},\n"
                    << "  \"buffers\": [{\"uri\": \"asset.bin\", \"byteLength\": " << buffer.size()
                    << "}],\n"
                    << "  \"bufferViews\": [\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_pos_offset
                    << ", \"byteLength\": " << skinned_pos_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_joints_offset
                    << ", \"byteLength\": " << skinned_joints_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_weights_offset
                    << ", \"byteLength\": " << skinned_weights_length << ", \"target\": 34962},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << skinned_idx_offset
                    << ", \"byteLength\": " << skinned_idx_length << ", \"target\": 34963},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << ibm_offset
                    << ", \"byteLength\": " << ibm_length << "},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << anim_times_offset
                    << ", \"byteLength\": " << anim_times_length << "},\n"
                    << "    {\"buffer\": 0, \"byteOffset\": " << anim_translation_offset
                    << ", \"byteLength\": " << anim_translation_length << "}\n"
                    << "  ],\n"
                    << "  \"accessors\": [\n"
                    << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": "
                       "\"VEC3\"},\n"
                    << "    {\"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": "
                       "\"VEC4\"},\n"
                    << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": "
                       "\"VEC4\"},\n"
                    << "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": "
                       "\"SCALAR\"},\n"
                    << "    {\"bufferView\": 4, \"componentType\": 5126, \"count\": 1, \"type\": "
                       "\"MAT4\"},\n"
                    << "    {\"bufferView\": 5, \"componentType\": 5126, \"count\": 2, \"type\": "
                       "\"SCALAR\"},\n"
                    << "    {\"bufferView\": 6, \"componentType\": 5126, \"count\": 6, \"type\": "
                       "\"VEC3\"}\n"
                    << "  ],\n"
                    << "  \"meshes\": [\n"
                    << "    {\"primitives\": [{\"attributes\": {\"POSITION\": 0, \"JOINTS_0\": 1, "
                       "\"WEIGHTS_0\": 2}, \"indices\": 3, \"mode\": 4}]}\n"
                    << "  ],\n"
                    << "  \"nodes\": [\n"
                    << "    {\"name\": \"joint0\"},\n"
                    << "    {\"mesh\": 0, \"skin\": 0}\n"
                    << "  ],\n"
                    << "  \"skins\": [{\"joints\": [0], \"inverseBindMatrices\": 4}],\n"
                    << "  \"animations\": [{\n"
                    << "    \"samplers\": [{\"input\": 5, \"output\": 6, \"interpolation\": "
                       "\"CUBICSPLINE\"}],\n"
                    << "    \"channels\": [{\"sampler\": 0, \"target\": {\"node\": 0, \"path\": "
                       "\"translation\"}}]\n"
                    << "  }],\n"
                    << "  \"scenes\": [{\"nodes\": [1]}],\n"
                    << "  \"scene\": 0\n"
                    << "}\n";
        ASSERT_TRUE(gltf_stream.good());
    }

    const AnimatedGltfLoadResult loaded = load_from_file(gltf_path.string());
    std::filesystem::remove_all(temp_dir);

    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("CUBICSPLINE"), std::string::npos);
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

TEST(AnimatedGltfPoseTests, SamplesStepInterpolation) {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.push_back(SkeletonJoint{});
    asset.bind_local_transforms.push_back(Transform{});

    AnimationClip clip;
    clip.name = "step_move";
    clip.duration_seconds = 1.0F;
    clip.joint_tracks.resize(1U);
    clip.joint_tracks[0].translation_interpolation = TrackInterpolation::Step;
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    std::string error;
    ASSERT_TRUE(evaluate_clip_pose(asset, 0U, 0.5F, pose, &error)) << error;
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 0.0F, 1.0e-4F);
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

TEST(AnimatedGltfPoseTests, HandlesNonTopologicalJointOrder) {
    AnimatedGltfAsset asset;
    // Joint 0 is the child, joint 1 is the parent.
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = 1 });
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = -1 });
    asset.bind_local_transforms.resize(2U);
    asset.bind_local_transforms[0].position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F };
    asset.bind_local_transforms[1].position = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F };

    AnimationClip clip;
    clip.duration_seconds = 1.0F;
    clip.joint_tracks.resize(2U);
    // Parent stays at x=2, child local is x=1 so expected child global x=3.
    clip.joint_tracks[1].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[1].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(evaluate_clip_pose(asset, 0U, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 3.0F, 1.0e-4F);
}

TEST(AnimatedGltfPoseTests, AppliesBindPrefixMatricesForNonJointAncestors) {
    AnimatedGltfAsset asset;
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = -1 });
    asset.skeleton.joints.push_back(SkeletonJoint{ .parent_index = 0 });
    asset.bind_local_transforms.resize(2U);
    asset.bind_local_transforms[1].position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F };
    asset.bind_prefix_matrices.resize(2U, Mat4::identity());
    asset.bind_prefix_matrices[1] =
        Mat4::from_position_scale_quat(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity());

    AnimationClip clip;
    clip.duration_seconds = 1.0F;
    clip.joint_tracks.resize(2U);
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(evaluate_clip_pose(asset, 0U, 0.25F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 3.0F, 1.0e-4F);
}

} // namespace isla::client::animated_gltf
