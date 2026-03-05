#include "client_app_geometry_utils.hpp"

#include "absl/log/log.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <span>
#include <vector>

namespace isla::client {
namespace {

std::vector<Triangle> make_unit_cube_triangles() {
    const Vec3 p000{ .x = -0.5F, .y = -0.5F, .z = -0.5F };
    const Vec3 p001{ .x = -0.5F, .y = -0.5F, .z = 0.5F };
    const Vec3 p010{ .x = -0.5F, .y = 0.5F, .z = -0.5F };
    const Vec3 p011{ .x = -0.5F, .y = 0.5F, .z = 0.5F };
    const Vec3 p100{ .x = 0.5F, .y = -0.5F, .z = -0.5F };
    const Vec3 p101{ .x = 0.5F, .y = -0.5F, .z = 0.5F };
    const Vec3 p110{ .x = 0.5F, .y = 0.5F, .z = -0.5F };
    const Vec3 p111{ .x = 0.5F, .y = 0.5F, .z = 0.5F };
    return {
        Triangle{ .a = p001, .b = p101, .c = p111 }, Triangle{ .a = p001, .b = p111, .c = p011 },
        Triangle{ .a = p100, .b = p000, .c = p010 }, Triangle{ .a = p100, .b = p010, .c = p110 },
        Triangle{ .a = p000, .b = p001, .c = p011 }, Triangle{ .a = p000, .b = p011, .c = p010 },
        Triangle{ .a = p101, .b = p100, .c = p110 }, Triangle{ .a = p101, .b = p110, .c = p111 },
        Triangle{ .a = p010, .b = p011, .c = p111 }, Triangle{ .a = p010, .b = p111, .c = p110 },
        Triangle{ .a = p000, .b = p100, .c = p101 }, Triangle{ .a = p000, .b = p101, .c = p001 },
    };
}

std::vector<Triangle> make_unit_octahedron_triangles() {
    const Vec3 px{ .x = 1.0F, .y = 0.0F, .z = 0.0F };
    const Vec3 nx{ .x = -1.0F, .y = 0.0F, .z = 0.0F };
    const Vec3 py{ .x = 0.0F, .y = 1.0F, .z = 0.0F };
    const Vec3 ny{ .x = 0.0F, .y = -1.0F, .z = 0.0F };
    const Vec3 pz{ .x = 0.0F, .y = 0.0F, .z = 1.0F };
    const Vec3 nz{ .x = 0.0F, .y = 0.0F, .z = -1.0F };
    return {
        Triangle{ .a = py, .b = px, .c = pz }, Triangle{ .a = py, .b = pz, .c = nx },
        Triangle{ .a = py, .b = nx, .c = nz }, Triangle{ .a = py, .b = nz, .c = px },
        Triangle{ .a = ny, .b = pz, .c = px }, Triangle{ .a = ny, .b = nx, .c = pz },
        Triangle{ .a = ny, .b = nz, .c = nx }, Triangle{ .a = ny, .b = px, .c = nz },
    };
}

Vec3 scaled_vec3(const Vec3& value, const Vec3& scale) {
    return Vec3{ .x = value.x * scale.x, .y = value.y * scale.y, .z = value.z * scale.z };
}

std::vector<Triangle> scale_triangles(std::span<const Triangle> triangles, const Vec3& scale) {
    std::vector<Triangle> out;
    out.reserve(triangles.size());
    for (const Triangle& tri : triangles) {
        out.push_back(Triangle{
            .a = scaled_vec3(tri.a, scale),
            .b = scaled_vec3(tri.b, scale),
            .c = scaled_vec3(tri.c, scale),
        });
    }
    return out;
}

} // namespace

Transform make_visible_object_transform_for_meshes(std::span<const MeshData> meshes) {
    bool has_bounds = false;
    Vec3 aggregate_center{};
    float aggregate_radius = 0.0F;
    for (const MeshData& mesh : meshes) {
        const BoundingSphere bounds = mesh.local_bounds();
        if (!std::isfinite(bounds.center.x) || !std::isfinite(bounds.center.y) ||
            !std::isfinite(bounds.center.z) || !std::isfinite(bounds.radius) ||
            bounds.radius <= 0.0F) {
            continue;
        }
        if (!has_bounds) {
            aggregate_center = bounds.center;
            aggregate_radius = bounds.radius;
            has_bounds = true;
            continue;
        }
        const Vec3 delta{
            .x = bounds.center.x - aggregate_center.x,
            .y = bounds.center.y - aggregate_center.y,
            .z = bounds.center.z - aggregate_center.z,
        };
        const float center_distance =
            std::sqrt((delta.x * delta.x) + (delta.y * delta.y) + (delta.z * delta.z));
        if (aggregate_radius >= center_distance + bounds.radius) {
            continue;
        }
        if (bounds.radius >= center_distance + aggregate_radius) {
            aggregate_center = bounds.center;
            aggregate_radius = bounds.radius;
            continue;
        }
        const float new_radius = (center_distance + aggregate_radius + bounds.radius) * 0.5F;
        if (center_distance > 1.0e-6F) {
            const float center_shift = (new_radius - aggregate_radius) / center_distance;
            aggregate_center.x += delta.x * center_shift;
            aggregate_center.y += delta.y * center_shift;
            aggregate_center.z += delta.z * center_shift;
        }
        aggregate_radius = new_radius;
    }
    if (!has_bounds || !std::isfinite(aggregate_radius) || aggregate_radius <= 0.0F) {
        return {};
    }
    constexpr float kTargetRadius = 1.0F;
    const float scale = kTargetRadius / std::max(aggregate_radius, 1.0e-4F);
    Transform transform{};
    transform.rotation =
        Quat::from_axis_angle(Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F }, std::numbers::pi_v<float>);
    transform.scale = Vec3{ .x = scale, .y = scale, .z = scale };
    transform.position = Vec3{ .x = -aggregate_center.x * scale,
                               .y = -aggregate_center.y * scale,
                               .z = -aggregate_center.z * scale };
    return transform;
}

std::size_t find_clip_index_by_name(const animated_gltf::AnimatedGltfAsset& asset,
                                    const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return 0U;
    }
    const auto it =
        std::ranges::find_if(asset.clips, [name](const auto& clip) { return clip.name == name; });
    return static_cast<std::size_t>(std::distance(asset.clips.cbegin(), it));
}

std::vector<SkinnedMeshVertex>
make_render_skinned_vertices(const animated_gltf::SkinnedPrimitive& primitive) {
    std::vector<SkinnedMeshVertex> vertices;
    vertices.reserve(primitive.vertices.size());
    for (const animated_gltf::SkinnedVertex& vertex : primitive.vertices) {
        vertices.push_back(SkinnedMeshVertex{
            .position = vertex.position,
            .normal = vertex.normal,
            .uv = vertex.uv,
            .joints = vertex.joints,
            .weights = vertex.weights,
        });
    }
    return vertices;
}

std::vector<Triangle>
make_triangles_from_skinned_geometry(std::span<const SkinnedMeshVertex> vertices,
                                     std::span<const std::uint32_t> indices) {
    std::vector<Triangle> triangles;
    if ((indices.size() % 3U) != 0U) {
        return triangles;
    }
    triangles.reserve(indices.size() / 3U);
    for (std::size_t i = 0U; i < indices.size(); i += 3U) {
        const std::uint32_t ia = indices[i];
        const std::uint32_t ib = indices[i + 1U];
        const std::uint32_t ic = indices[i + 2U];
        if (static_cast<std::size_t>(ia) >= vertices.size() ||
            static_cast<std::size_t>(ib) >= vertices.size() ||
            static_cast<std::size_t>(ic) >= vertices.size()) {
            continue;
        }
        const SkinnedMeshVertex& a = vertices[ia];
        const SkinnedMeshVertex& b = vertices[ib];
        const SkinnedMeshVertex& c = vertices[ic];
        triangles.push_back(Triangle{
            .a = a.position,
            .b = b.position,
            .c = c.position,
            .uv_a = a.uv,
            .uv_b = b.uv,
            .uv_c = c.uv,
        });
    }
    return triangles;
}

std::vector<Mat4> make_remapped_skin_palette(std::span<const Mat4> global_skin_matrices,
                                             std::span<const std::uint16_t> global_joints) {
    std::vector<Mat4> palette(global_joints.size(), Mat4::identity());
    for (std::size_t local_joint = 0U; local_joint < global_joints.size(); ++local_joint) {
        const std::size_t global_joint = global_joints[local_joint];
        if (global_joint >= global_skin_matrices.size()) {
            LOG_EVERY_N_SEC(WARNING, 2.0)
                << "ClientApp: remapped GPU palette references out-of-range global joint index "
                << global_joint << " for skin matrix count " << global_skin_matrices.size()
                << "; using identity";
            continue;
        }
        palette[local_joint] = global_skin_matrices[global_joint];
    }
    return palette;
}

Mat4 make_collider_local_matrix(const pmx_physics_sidecar::Collider& collider) {
    const float kDegToRad = std::numbers::pi_v<float> / 180.0F;
    const Mat4 rotation =
        multiply(multiply(Mat4::rotation_z(collider.rotation_euler_deg.z * kDegToRad),
                          Mat4::rotation_y(collider.rotation_euler_deg.y * kDegToRad)),
                 Mat4::rotation_x(collider.rotation_euler_deg.x * kDegToRad));
    return multiply(Mat4::translation(collider.offset), rotation);
}

std::vector<Triangle> make_triangles_for_collider(const pmx_physics_sidecar::Collider& collider) {
    if (collider.shape == pmx_physics_sidecar::ColliderShape::Sphere) {
        const Vec3 scale{ .x = collider.radius, .y = collider.radius, .z = collider.radius };
        return scale_triangles(make_unit_octahedron_triangles(), scale);
    }
    if (collider.shape == pmx_physics_sidecar::ColliderShape::Capsule) {
        const Vec3 scale{
            .x = collider.radius * 2.0F,
            .y = collider.height + (collider.radius * 2.0F),
            .z = collider.radius * 2.0F,
        };
        // TODO(isla): Replace this box proxy with a low-poly capsule mesh (cylinder + hemispheres)
        // for better visual fidelity once Phase 5 proxy-shape refinement is scheduled.
        return scale_triangles(make_unit_cube_triangles(), scale);
    }
    return scale_triangles(make_unit_cube_triangles(), collider.size);
}

void apply_matrix_to_triangles_in_place(std::span<const Triangle> source, const Mat4& matrix,
                                        std::vector<Triangle>& destination) {
    destination.resize(source.size());
    for (std::size_t i = 0U; i < source.size(); ++i) {
        destination[i].a = transform_point(matrix, source[i].a);
        destination[i].b = transform_point(matrix, source[i].b);
        destination[i].c = transform_point(matrix, source[i].c);
    }
}

std::vector<std::string> collect_joint_names(const animated_gltf::AnimatedGltfAsset& asset) {
    std::vector<std::string> names;
    names.reserve(asset.skeleton.joints.size());
    for (const animated_gltf::SkeletonJoint& joint : asset.skeleton.joints) {
        names.push_back(joint.name);
    }
    return names;
}

} // namespace isla::client
