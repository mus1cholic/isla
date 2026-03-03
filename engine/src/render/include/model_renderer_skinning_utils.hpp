#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/render/render_world.hpp"

namespace isla::client {

inline constexpr std::size_t kMaxGpuSkinningJoints = 64U;

enum class SkinningProgramPath {
    StaticMesh = 0,
    SkinnedMesh,
};

struct SkinningProgramDecisionInputs {
    bool mesh_is_skinned = false;
    bool has_skin_palette = false;
    bool gpu_skinning_supported = false;
    bool skinned_program_valid = false;
};

[[nodiscard]] SkinningProgramPath
choose_skinning_program_path(const SkinningProgramDecisionInputs& inputs);

// Fills destination with identity matrices, then copies as much source palette as fits.
// Returns true when source palette was truncated.
[[nodiscard]] bool fill_skin_palette_upload_buffer(std::span<const Mat4> source_palette,
                                                   std::span<Mat4> upload_buffer,
                                                   std::size_t* copied_count = nullptr);

struct GpuSkinningPartition {
    MeshData::SkinnedVertexList vertices;
    MeshData::IndexList indices;
    std::vector<std::uint16_t> global_joint_palette;
};

[[nodiscard]] bool build_gpu_skinning_partitions(std::span<const SkinnedMeshVertex> vertices,
                                                 std::span<const std::uint32_t> indices,
                                                 std::size_t max_palette_joints,
                                                 std::vector<GpuSkinningPartition>& out_partitions,
                                                 std::string* error_message = nullptr);

} // namespace isla::client
