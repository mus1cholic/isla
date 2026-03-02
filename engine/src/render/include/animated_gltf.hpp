#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/math/quat.hpp"
#include "isla/engine/math/transform.hpp"

namespace isla::client::animated_gltf {

struct SkinnedVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
    std::array<std::uint16_t, 4U> joints{ 0U, 0U, 0U, 0U };
    std::array<float, 4U> weights{ 1.0F, 0.0F, 0.0F, 0.0F };
};

struct SkinnedPrimitive {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct SkeletonJoint {
    std::string name;
    int parent_index = -1;
    Mat4 inverse_bind_matrix = Mat4::identity();
};

struct Skeleton {
    std::vector<SkeletonJoint> joints;
};

struct Vec3Keyframe {
    float time_seconds = 0.0F;
    Vec3 value{};
};

struct QuatKeyframe {
    float time_seconds = 0.0F;
    Quat value = Quat::identity();
};

enum class TrackInterpolation {
    Linear = 0,
    Step,
};

struct JointAnimationTrack {
    std::vector<Vec3Keyframe> translations;
    std::vector<QuatKeyframe> rotations;
    std::vector<Vec3Keyframe> scales;
    TrackInterpolation translation_interpolation = TrackInterpolation::Linear;
    TrackInterpolation rotation_interpolation = TrackInterpolation::Linear;
    TrackInterpolation scale_interpolation = TrackInterpolation::Linear;
};

struct AnimationClip {
    std::string name;
    float duration_seconds = 0.0F;
    std::vector<JointAnimationTrack> joint_tracks;
};

struct AnimatedGltfAsset {
    std::vector<SkinnedPrimitive> primitives;
    Skeleton skeleton;
    std::vector<Transform> bind_local_transforms;
    // Static transform chain between nearest joint-parent and this joint.
    // Local joint animation is evaluated in bind_local_transforms, then prefixed by this matrix.
    std::vector<Mat4> bind_prefix_matrices;
    std::vector<AnimationClip> clips;
};

struct AnimatedGltfLoadResult {
    bool ok = false;
    AnimatedGltfAsset asset;
    std::string error_message;
};

struct EvaluatedPose {
    std::vector<Mat4> global_joint_matrices;
    std::vector<Mat4> skin_matrices;
};

[[nodiscard]] AnimatedGltfLoadResult load_from_file(std::string_view asset_path);

[[nodiscard]] bool evaluate_clip_pose(const AnimatedGltfAsset& asset, std::size_t clip_index,
                                      float time_seconds, EvaluatedPose& out_pose,
                                      std::string* error_message = nullptr);

} // namespace isla::client::animated_gltf
