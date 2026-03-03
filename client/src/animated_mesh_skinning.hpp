#pragma once

#include <vector>

#include "engine/src/render/include/animated_gltf.hpp"
#include "isla/engine/math/mat4.hpp"
#include "isla/engine/render/render_types.hpp"

namespace isla::client::animated_mesh_skinning {

[[nodiscard]] std::vector<Triangle>
make_triangles_from_skinned_primitive(const animated_gltf::SkinnedPrimitive& primitive,
                                      const std::vector<Mat4>* skin_matrices);

} // namespace isla::client::animated_mesh_skinning
