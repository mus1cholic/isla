#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "isla/engine/math/math_types.hpp"

namespace isla::client::pmx_physics_sidecar {

inline constexpr std::string_view kExpectedSchemaVersion = "1.0.0";
inline constexpr std::uint32_t kMaxCollisionLayerIndex = 31U;
inline constexpr std::size_t kMaxSidecarFileSizeBytes = 10U * 1024U * 1024U;
inline constexpr std::size_t kMaxCollisionLayers = 256U;
inline constexpr std::size_t kMaxColliders = 16384U;
inline constexpr std::size_t kMaxConstraints = 16384U;
inline constexpr std::size_t kMaxStringLengthBytes = 256U;

enum class ColliderShape {
    Sphere = 0,
    Capsule,
    Box,
};

enum class ConstraintType {
    Fixed = 0,
    Hinge,
    ConeTwist,
};

struct CollisionLayer {
    std::uint32_t index = 0U;
    std::string name;
};

struct Collider {
    std::string id;
    std::string bone_name;
    ColliderShape shape = ColliderShape::Sphere;
    Vec3 offset{};
    Vec3 rotation_euler_deg{};
    bool is_trigger = false;
    std::uint32_t layer = 0U;
    std::uint32_t mask = 0U;
    float radius = 0.0F;
    float height = 0.0F;
    Vec3 size{};
};

struct Constraint {
    std::string id;
    std::string bone_a_name;
    std::string bone_b_name;
    ConstraintType type = ConstraintType::Fixed;
};

struct SidecarData {
    std::vector<CollisionLayer> collision_layers;
    std::vector<Collider> colliders;
    std::vector<Constraint> constraints;
};

struct SidecarLoadResult {
    bool ok = false;
    SidecarData sidecar;
    std::vector<std::string> warnings;
    std::string error_message;
};

[[nodiscard]] SidecarLoadResult load_from_file(std::string_view sidecar_path,
                                               std::span<const std::string> joint_names = {});

} // namespace isla::client::pmx_physics_sidecar
