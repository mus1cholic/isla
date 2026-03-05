#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/render/render_types.hpp"

namespace isla::client::animated_mesh_skinning {
struct PrimitiveSkinningWorkspace;
}

namespace isla::client::internal {

struct AnimatedMeshBinding {
    std::size_t mesh_id = 0U;
    std::size_t primitive_index = 0U;
    std::vector<std::uint16_t> gpu_palette_global_joints;
    std::unique_ptr<animated_mesh_skinning::PrimitiveSkinningWorkspace> skinning_workspace;

    AnimatedMeshBinding();
    ~AnimatedMeshBinding();
    AnimatedMeshBinding(const AnimatedMeshBinding&) = delete;
    AnimatedMeshBinding& operator=(const AnimatedMeshBinding&) = delete;
    AnimatedMeshBinding(AnimatedMeshBinding&&) noexcept;
    AnimatedMeshBinding& operator=(AnimatedMeshBinding&&) noexcept;
};

struct PhysicsColliderBinding {
    std::size_t mesh_id = 0U;
    std::size_t bone_index = 0U;
    Mat4 bone_local_collider_matrix = Mat4::identity();
    std::vector<Triangle> bind_local_triangles;
};

} // namespace isla::client::internal
