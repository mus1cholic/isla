#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "engine/src/render/include/animated_gltf.hpp"
#include "shared/src/test_runfiles.hpp"

namespace isla::client::animated_gltf {

class AnimatedGltfTest : public ::testing::Test {
  protected:
    std::filesystem::path make_temp_dir() {
        static std::mt19937_64 rng(std::random_device{}());
        static std::mutex rng_mutex;
        const std::filesystem::path temp_root = std::filesystem::temp_directory_path();

        for (int attempt = 0; attempt < 16; ++attempt) {
            std::uint64_t random_suffix = 0U;
            {
                const std::lock_guard<std::mutex> lock(rng_mutex);
                random_suffix = rng();
            }
            const std::filesystem::path temp_dir =
                temp_root / ("isla_animated_gltf_" + std::to_string(random_suffix));
            std::error_code ec;
            if (std::filesystem::create_directory(temp_dir, ec)) {
                temp_dirs_.push_back(temp_dir);
                return temp_dir;
            }
        }

        ADD_FAILURE() << "failed to create unique temporary directory after retries";
        const std::filesystem::path fallback =
            temp_root / ("isla_animated_gltf_fallback_" + std::to_string(temp_dirs_.size()));
        std::error_code fallback_ec;
        std::filesystem::create_directories(fallback, fallback_ec);
        temp_dirs_.push_back(fallback);
        return fallback;
    }

    static AnimatedGltfAsset make_single_joint_asset() {
        AnimatedGltfAsset asset;
        asset.skeleton.joints.push_back(SkeletonJoint{});
        asset.bind_local_transforms.push_back(Transform{});
        return asset;
    }

    static AnimationClip make_single_joint_clip(const std::string& name, float duration_seconds) {
        AnimationClip clip;
        clip.name = name;
        clip.duration_seconds = duration_seconds;
        clip.joint_tracks.resize(1U);
        return clip;
    }

    static void align4(std::vector<std::uint8_t>& buffer) {
        while ((buffer.size() % 4U) != 0U) {
            buffer.push_back(0U);
        }
    }

    static void append_f32(std::vector<std::uint8_t>& buffer, float value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    }

    static void append_u16(std::vector<std::uint8_t>& buffer, std::uint16_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
    }

    static ::testing::AssertionResult write_binary_file(
        const std::filesystem::path& path, const std::vector<std::uint8_t>& buffer) {
        std::ofstream bin_stream(path, std::ios::binary);
        if (!bin_stream.is_open()) {
            return ::testing::AssertionFailure() << "failed to open binary file: " << path;
        }
        bin_stream.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(buffer.size()));
        if (!bin_stream.good()) {
            return ::testing::AssertionFailure() << "failed to write binary file: " << path;
        }
        return ::testing::AssertionSuccess();
    }

    static ::testing::AssertionResult write_text_file(const std::filesystem::path& path,
                                                      const std::string& text) {
        std::ofstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            return ::testing::AssertionFailure() << "failed to open text file: " << path;
        }
        stream << text;
        if (!stream.good()) {
            return ::testing::AssertionFailure() << "failed to write text file: " << path;
        }
        return ::testing::AssertionSuccess();
    }

    static ::testing::AssertionResult assert_evaluate_clip_pose_ok(
        const AnimatedGltfAsset& asset, float sample_time_seconds, EvaluatedPose& pose,
        std::string* error) {
        if (!evaluate_clip_pose(asset, 0U, sample_time_seconds, pose, error)) {
            return ::testing::AssertionFailure() << (error != nullptr ? *error : "");
        }
        return ::testing::AssertionSuccess();
    }

    static ::testing::AssertionResult assert_evaluate_clip_pose_ok(
        const AnimatedGltfAsset& asset, float sample_time_seconds, EvaluatedPose& pose,
        std::string* error, ClipPlaybackMode playback_mode) {
        if (!evaluate_clip_pose(asset, 0U, sample_time_seconds, pose, error, playback_mode)) {
            return ::testing::AssertionFailure() << (error != nullptr ? *error : "");
        }
        return ::testing::AssertionSuccess();
    }

    void TearDown() override {
        for (const std::filesystem::path& temp_dir : temp_dirs_) {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }
    }

  private:
    std::vector<std::filesystem::path> temp_dirs_;
};

TEST_F(AnimatedGltfTest, RejectsNonSkinnedGltf) {
    const std::string path =
        isla::shared::test::runfile_path("engine/src/render/testdata/triangle_embedded.gltf");
    const AnimatedGltfLoadResult loaded = load_from_file(path);
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.error_message.empty());
}

TEST_F(AnimatedGltfTest, LoadsOnlyPrimitivesAttachedToSelectedSkin) {
    const std::filesystem::path temp_dir = make_temp_dir();

    const std::filesystem::path bin_path = temp_dir / "asset.bin";
    const std::filesystem::path gltf_path = temp_dir / "asset.gltf";

    std::vector<std::uint8_t> buffer;

    const std::size_t unskinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t unskinned_pos_length = buffer.size() - unskinned_pos_offset;

    const std::size_t unskinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(buffer, value);
    }
    const std::size_t unskinned_idx_length = buffer.size() - unskinned_idx_offset;

    align4(buffer);
    const std::size_t skinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t skinned_pos_length = buffer.size() - skinned_pos_offset;

    const std::size_t skinned_joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (std::uint16_t value : { 0U, 0U, 0U, 0U }) {
            append_u16(buffer, value);
        }
    }
    const std::size_t skinned_joints_length = buffer.size() - skinned_joints_offset;

    const std::size_t skinned_weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (float value : { 1.0F, 0.0F, 0.0F, 0.0F }) {
            append_f32(buffer, value);
        }
    }
    const std::size_t skinned_weights_length = buffer.size() - skinned_weights_offset;

    const std::size_t skinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(buffer, value);
    }
    const std::size_t skinned_idx_length = buffer.size() - skinned_idx_offset;

    align4(buffer);
    const std::size_t ibm_offset = buffer.size();
    for (float value : { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 1.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    ASSERT_TRUE(write_binary_file(bin_path, buffer));

    {
        std::ostringstream gltf;
        gltf << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << R"(  "buffers": [{"uri": "asset.bin", "byteLength": )" << buffer.size() << "}],\n"
             << "  \"bufferViews\": [\n"
             << R"(    {"buffer": 0, "byteOffset": )" << unskinned_pos_offset
             << ", \"byteLength\": " << unskinned_pos_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << unskinned_idx_offset
             << ", \"byteLength\": " << unskinned_idx_length << ", \"target\": 34963},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_pos_offset
             << ", \"byteLength\": " << skinned_pos_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_joints_offset
             << ", \"byteLength\": " << skinned_joints_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_weights_offset
             << ", \"byteLength\": " << skinned_weights_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_idx_offset
             << ", \"byteLength\": " << skinned_idx_length << ", \"target\": 34963},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << ibm_offset
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
        ASSERT_TRUE(write_text_file(gltf_path, gltf.str()));
    }

    const AnimatedGltfLoadResult loaded = load_from_file(gltf_path.string());

    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.asset.primitives.size(), 1U);
    ASSERT_EQ(loaded.asset.primitives[0].vertices.size(), 3U);
    ASSERT_EQ(loaded.asset.primitives[0].indices.size(), 3U);
}

TEST_F(AnimatedGltfTest, RejectsCubicSplineInterpolation) {
    const std::filesystem::path temp_dir = make_temp_dir();

    const std::filesystem::path bin_path = temp_dir / "asset.bin";
    const std::filesystem::path gltf_path = temp_dir / "asset.gltf";

    std::vector<std::uint8_t> buffer;

    const std::size_t skinned_pos_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t skinned_pos_length = buffer.size() - skinned_pos_offset;

    const std::size_t skinned_joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (std::uint16_t value : { 0U, 0U, 0U, 0U }) {
            append_u16(buffer, value);
        }
    }
    const std::size_t skinned_joints_length = buffer.size() - skinned_joints_offset;

    const std::size_t skinned_weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        for (float value : { 1.0F, 0.0F, 0.0F, 0.0F }) {
            append_f32(buffer, value);
        }
    }
    const std::size_t skinned_weights_length = buffer.size() - skinned_weights_offset;

    const std::size_t skinned_idx_offset = buffer.size();
    for (std::uint16_t value : { 0U, 1U, 2U }) {
        append_u16(buffer, value);
    }
    const std::size_t skinned_idx_length = buffer.size() - skinned_idx_offset;

    align4(buffer);
    const std::size_t ibm_offset = buffer.size();
    for (float value : { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 1.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    const std::size_t anim_times_offset = buffer.size();
    for (float value : { 0.0F, 1.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t anim_times_length = buffer.size() - anim_times_offset;

    const std::size_t anim_translation_offset = buffer.size();
    for (float value : { 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                         0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F }) {
        append_f32(buffer, value);
    }
    const std::size_t anim_translation_length = buffer.size() - anim_translation_offset;

    ASSERT_TRUE(write_binary_file(bin_path, buffer));

    {
        std::ostringstream gltf;
        gltf << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << R"(  "buffers": [{"uri": "asset.bin", "byteLength": )" << buffer.size() << "}],\n"
             << "  \"bufferViews\": [\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_pos_offset
             << ", \"byteLength\": " << skinned_pos_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_joints_offset
             << ", \"byteLength\": " << skinned_joints_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_weights_offset
             << ", \"byteLength\": " << skinned_weights_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << skinned_idx_offset
             << ", \"byteLength\": " << skinned_idx_length << ", \"target\": 34963},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << ibm_offset
             << ", \"byteLength\": " << ibm_length << "},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << anim_times_offset
             << ", \"byteLength\": " << anim_times_length << "},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << anim_translation_offset
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
        ASSERT_TRUE(write_text_file(gltf_path, gltf.str()));
    }

    const AnimatedGltfLoadResult loaded = load_from_file(gltf_path.string());

    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("CUBICSPLINE"), std::string::npos);
}

TEST_F(AnimatedGltfTest, InterpolatesSingleJointTranslation) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("move", 1.0F);
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    std::string error;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, &error));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 1.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, SamplesExactClipEndByPlaybackMode) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("end_mode", 1.0F);
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 1.0F, pose, nullptr, ClipPlaybackMode::Loop));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 0.0F, 1.0e-4F);

    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 1.0F, pose, nullptr, ClipPlaybackMode::Clamp));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 2.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, SamplesStepInterpolation) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("step_move", 1.0F);
    clip.joint_tracks[0].translation_interpolation = TrackInterpolation::Step;
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 0.5F, .value = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } });
    clip.joint_tracks[0].translations.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    std::string error;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.25F, pose, &error));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 0.0F, 1.0e-4F);

    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, &error));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 1.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, InterpolatesRotationLinearly) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("", 1.0F);
    clip.joint_tracks[0].rotations.push_back(
        QuatKeyframe{ .time_seconds = 0.0F, .value = Quat::identity() });
    clip.joint_tracks[0].rotations.push_back(QuatKeyframe{
        .time_seconds = 1.0F,
        .value = Quat::from_axis_angle(Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F }, 3.14159265F) });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);

    // Midway between identity and 180deg Z should be ~90deg Z.
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 0.0F, 1.0e-4F);
    EXPECT_NEAR(std::abs(pose.global_joint_matrices[0].elements[1]), 1.0F, 1.0e-4F);
    EXPECT_NEAR(std::abs(pose.global_joint_matrices[0].elements[4]), 1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[1] +
                    pose.global_joint_matrices[0].elements[4],
                0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 0.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, SamplesRotationStepInterpolationIncludingExactKeyTime) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("", 1.0F);
    clip.joint_tracks[0].rotation_interpolation = TrackInterpolation::Step;
    clip.joint_tracks[0].rotations.push_back(
        QuatKeyframe{ .time_seconds = 0.0F, .value = Quat::identity() });
    clip.joint_tracks[0].rotations.push_back(QuatKeyframe{
        .time_seconds = 0.5F,
        .value = Quat::from_axis_angle(Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F }, 1.57079633F) });
    clip.joint_tracks[0].rotations.push_back(QuatKeyframe{
        .time_seconds = 1.0F,
        .value = Quat::from_axis_angle(Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F }, 3.14159265F) });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.25F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[1], 0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[4], 0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 1.0F, 1.0e-4F);

    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[1], 1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[4], -1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 0.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, InterpolatesScaleLinearly) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("", 1.0F);
    clip.joint_tracks[0].scales.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F } });
    clip.joint_tracks[0].scales.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 3.0F, .y = 2.0F, .z = 1.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 1.5F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[10], 1.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, SamplesScaleStepInterpolationIncludingExactKeyTime) {
    AnimatedGltfAsset asset = make_single_joint_asset();

    AnimationClip clip = make_single_joint_clip("", 1.0F);
    clip.joint_tracks[0].scale_interpolation = TrackInterpolation::Step;
    clip.joint_tracks[0].scales.push_back(
        Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F } });
    clip.joint_tracks[0].scales.push_back(
        Vec3Keyframe{ .time_seconds = 0.5F, .value = Vec3{ .x = 2.0F, .y = 3.0F, .z = 4.0F } });
    clip.joint_tracks[0].scales.push_back(
        Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 5.0F, .y = 6.0F, .z = 7.0F } });
    asset.clips.push_back(std::move(clip));

    EvaluatedPose pose;
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.25F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 1.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[10], 1.0F, 1.0e-4F);

    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 1U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[0], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[5], 3.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[10], 4.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, EvaluatesHierarchyAndSkinMatrices) {
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
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 1.0F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    ASSERT_EQ(pose.skin_matrices.size(), 2U);

    // Parent translated to x=2 at t=1. Child bind local x=1, so global child should be x=3.
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 3.0F, 1.0e-4F);
    EXPECT_NEAR(pose.skin_matrices[1].elements[12], 3.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, HandlesNonTopologicalJointOrder) {
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
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.5F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 2.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 3.0F, 1.0e-4F);
}

TEST_F(AnimatedGltfTest, AppliesBindPrefixMatricesForNonJointAncestors) {
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
    ASSERT_TRUE(assert_evaluate_clip_pose_ok(asset, 0.25F, pose, nullptr));
    ASSERT_EQ(pose.global_joint_matrices.size(), 2U);
    EXPECT_NEAR(pose.global_joint_matrices[0].elements[12], 0.0F, 1.0e-4F);
    EXPECT_NEAR(pose.global_joint_matrices[1].elements[12], 3.0F, 1.0e-4F);
}

} // namespace isla::client::animated_gltf
