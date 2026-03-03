#pragma once

#include <cstdint>
#include <vector>

#include "engine/src/render/include/animated_gltf.hpp"
#include "isla/engine/math/mat4.hpp"
#include "isla/engine/render/render_types.hpp"

namespace isla::client::animated_mesh_skinning {

struct PrimitiveSkinningWorkspace {
    std::vector<Vec3> skinned_positions;
    std::vector<std::uint32_t> triangle_vertex_indices;
};

[[nodiscard]] std::vector<Triangle>
make_triangles_from_skinned_primitive(const animated_gltf::SkinnedPrimitive& primitive,
                                      const std::vector<Mat4>* skin_matrices);

[[nodiscard]] std::vector<Triangle>
make_initial_triangles_and_workspace(const animated_gltf::SkinnedPrimitive& primitive,
                                     PrimitiveSkinningWorkspace* workspace);

void skin_primitive_in_place(const animated_gltf::SkinnedPrimitive& primitive,
                             const std::vector<Mat4>* skin_matrices,
                             PrimitiveSkinningWorkspace* workspace,
                             std::vector<Triangle>* triangles);

} // namespace isla::client::animated_mesh_skinning
