#pragma once

#include <cstddef>
#include <span>

#include "isla/engine/math/mat4.hpp"

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

} // namespace isla::client
