#include "engine/src/render/include/model_renderer_skinning_utils.hpp"

#include <algorithm>

namespace isla::client {

SkinningProgramPath choose_skinning_program_path(const SkinningProgramDecisionInputs& inputs) {
    if (!inputs.mesh_is_skinned || !inputs.has_skin_palette || !inputs.gpu_skinning_supported ||
        !inputs.skinned_program_valid) {
        return SkinningProgramPath::StaticMesh;
    }
    return SkinningProgramPath::SkinnedMesh;
}

bool fill_skin_palette_upload_buffer(std::span<const Mat4> source_palette,
                                     std::span<Mat4> upload_buffer,
                                     std::size_t* copied_count) {
    std::fill(upload_buffer.begin(), upload_buffer.end(), Mat4::identity());

    const std::size_t copy_count = std::min(source_palette.size(), upload_buffer.size());
    for (std::size_t i = 0U; i < copy_count; ++i) {
        upload_buffer[i] = source_palette[i];
    }
    if (copied_count != nullptr) {
        *copied_count = copy_count;
    }
    return source_palette.size() > upload_buffer.size();
}

} // namespace isla::client
