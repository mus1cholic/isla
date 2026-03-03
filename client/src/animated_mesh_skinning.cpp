#include "animated_mesh_skinning.hpp"

#include <cstddef>

#include "absl/log/log.h"

namespace isla::client::animated_mesh_skinning {

namespace {

Vec3 skin_point(const animated_gltf::SkinnedVertex& vertex, const std::vector<Mat4>* skin_matrices) {
    if (skin_matrices == nullptr || skin_matrices->empty()) {
        return vertex.position;
    }
    Vec3 blended{};
    float blended_weight = 0.0F;
    for (std::size_t i = 0U; i < vertex.joints.size(); ++i) {
        const float weight = vertex.weights[i];
        if (weight <= 0.0F) {
            continue;
        }
        const auto joint_index = static_cast<std::size_t>(vertex.joints[i]);
        if (joint_index >= skin_matrices->size()) {
            continue;
        }
        const Vec3 transformed = transform_point((*skin_matrices)[joint_index], vertex.position);
        blended.x += transformed.x * weight;
        blended.y += transformed.y * weight;
        blended.z += transformed.z * weight;
        blended_weight += weight;
    }
    if (blended_weight <= 1.0e-6F) {
        return vertex.position;
    }
    blended.x /= blended_weight;
    blended.y /= blended_weight;
    blended.z /= blended_weight;
    return blended;
}

bool workspace_topology_matches_primitive(const animated_gltf::SkinnedPrimitive& primitive,
                                          const PrimitiveSkinningWorkspace& workspace) {
    if ((workspace.triangle_vertex_indices.size() % 3U) != 0U) {
        return false;
    }
    for (const std::uint32_t vertex_index : workspace.triangle_vertex_indices) {
        if (static_cast<std::size_t>(vertex_index) >= primitive.vertices.size()) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<Triangle> make_initial_triangles_and_workspace(
    const animated_gltf::SkinnedPrimitive& primitive, PrimitiveSkinningWorkspace* workspace) {
    std::vector<Triangle> triangles;
    if (workspace == nullptr) {
        return triangles;
    }
    workspace->skinned_positions.clear();
    workspace->skinned_positions.reserve(primitive.vertices.size());
    workspace->triangle_vertex_indices.clear();
    workspace->triangle_vertex_indices.reserve((primitive.indices.size() / 3U) * 3U);
    triangles.reserve(primitive.indices.size() / 3U);

    for (std::size_t i = 0U; i + 2U < primitive.indices.size(); i += 3U) {
        const auto i0 = static_cast<std::size_t>(primitive.indices[i]);
        const auto i1 = static_cast<std::size_t>(primitive.indices[i + 1U]);
        const auto i2 = static_cast<std::size_t>(primitive.indices[i + 2U]);
        if (i0 >= primitive.vertices.size() || i1 >= primitive.vertices.size() ||
            i2 >= primitive.vertices.size()) {
            continue;
        }

        workspace->triangle_vertex_indices.push_back(static_cast<std::uint32_t>(i0));
        workspace->triangle_vertex_indices.push_back(static_cast<std::uint32_t>(i1));
        workspace->triangle_vertex_indices.push_back(static_cast<std::uint32_t>(i2));

        const animated_gltf::SkinnedVertex& v0 = primitive.vertices[i0];
        const animated_gltf::SkinnedVertex& v1 = primitive.vertices[i1];
        const animated_gltf::SkinnedVertex& v2 = primitive.vertices[i2];
        triangles.push_back(Triangle{
            .a = v0.position,
            .b = v1.position,
            .c = v2.position,
            .uv_a = v0.uv,
            .uv_b = v1.uv,
            .uv_c = v2.uv,
        });
    }

    return triangles;
}

void skin_primitive_in_place(const animated_gltf::SkinnedPrimitive& primitive,
                             const std::vector<Mat4>* skin_matrices,
                             PrimitiveSkinningWorkspace* workspace,
                             std::vector<Triangle>* triangles) {
    if (workspace == nullptr || triangles == nullptr) {
        return;
    }
    if (!workspace_topology_matches_primitive(primitive, *workspace)) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "AnimatedMeshSkinning: stale workspace topology detected; rebuilding "
               "triangle topology and workspace buffers";
        *triangles = make_initial_triangles_and_workspace(primitive, workspace);
    }
    workspace->skinned_positions.resize(primitive.vertices.size());
    for (std::size_t i = 0U; i < primitive.vertices.size(); ++i) {
        workspace->skinned_positions[i] = skin_point(primitive.vertices[i], skin_matrices);
    }

    const std::size_t triangle_count = workspace->triangle_vertex_indices.size() / 3U;
    if (triangles->size() != triangle_count) {
        triangles->resize(triangle_count);
    }

    for (std::size_t tri_index = 0U; tri_index < triangle_count; ++tri_index) {
        const std::size_t index_offset = tri_index * 3U;
        const std::size_t i0 =
            static_cast<std::size_t>(workspace->triangle_vertex_indices[index_offset]);
        const std::size_t i1 =
            static_cast<std::size_t>(workspace->triangle_vertex_indices[index_offset + 1U]);
        const std::size_t i2 =
            static_cast<std::size_t>(workspace->triangle_vertex_indices[index_offset + 2U]);

        Triangle& triangle = triangles->at(tri_index);
        triangle.a = workspace->skinned_positions[i0];
        triangle.b = workspace->skinned_positions[i1];
        triangle.c = workspace->skinned_positions[i2];
        triangle.uv_a = primitive.vertices[i0].uv;
        triangle.uv_b = primitive.vertices[i1].uv;
        triangle.uv_c = primitive.vertices[i2].uv;
    }
}

std::vector<Triangle> make_triangles_from_skinned_primitive(
    const animated_gltf::SkinnedPrimitive& primitive, const std::vector<Mat4>* skin_matrices) {
    PrimitiveSkinningWorkspace workspace;
    std::vector<Triangle> triangles = make_initial_triangles_and_workspace(primitive, &workspace);
    skin_primitive_in_place(primitive, skin_matrices, &workspace, &triangles);
    return triangles;
}

} // namespace isla::client::animated_mesh_skinning
