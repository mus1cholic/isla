#include "animated_mesh_skinning.hpp"

#include <cstddef>

namespace isla::client::animated_mesh_skinning {

std::vector<Triangle>
make_triangles_from_skinned_primitive(const animated_gltf::SkinnedPrimitive& primitive,
                                      const std::vector<Mat4>* skin_matrices) {
    std::vector<Triangle> triangles;
    triangles.reserve(primitive.indices.size() / 3U);

    auto skin_point = [&](const animated_gltf::SkinnedVertex& vertex) -> Vec3 {
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
            const Vec3 transformed =
                transform_point((*skin_matrices)[joint_index], vertex.position);
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
    };

    for (std::size_t i = 0U; i + 2U < primitive.indices.size(); i += 3U) {
        const auto i0 = static_cast<std::size_t>(primitive.indices[i]);
        const auto i1 = static_cast<std::size_t>(primitive.indices[i + 1U]);
        const auto i2 = static_cast<std::size_t>(primitive.indices[i + 2U]);
        if (i0 >= primitive.vertices.size() || i1 >= primitive.vertices.size() ||
            i2 >= primitive.vertices.size()) {
            continue;
        }
        const animated_gltf::SkinnedVertex& v0 = primitive.vertices[i0];
        const animated_gltf::SkinnedVertex& v1 = primitive.vertices[i1];
        const animated_gltf::SkinnedVertex& v2 = primitive.vertices[i2];
        triangles.push_back(Triangle{
            .a = skin_point(v0),
            .b = skin_point(v1),
            .c = skin_point(v2),
            .uv_a = v0.uv,
            .uv_b = v1.uv,
            .uv_c = v2.uv,
        });
    }

    return triangles;
}

} // namespace isla::client::animated_mesh_skinning
