#include "isla/engine/render/model_renderer_skinning_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace isla::client {

namespace {

constexpr float kWeightEpsilon = 1.0e-6F;

struct MutableGpuSkinningPartition {
    MeshData::SkinnedVertexList vertices;
    MeshData::IndexList indices;
    std::vector<std::uint16_t> global_joint_palette;
    std::unordered_map<std::uint16_t, std::uint16_t> global_to_local_joint;
    std::unordered_map<std::uint32_t, std::uint32_t> source_vertex_to_local;
};

void append_vertex_influence_joints(const SkinnedMeshVertex& vertex,
                                    std::array<std::uint16_t, 12U>& joints_buffer,
                                    std::size_t& joints_count) {
    bool has_non_zero_weight = false;
    for (std::size_t influence = 0U; influence < vertex.joints.size(); ++influence) {
        if (vertex.weights[influence] <= kWeightEpsilon) {
            continue;
        }
        has_non_zero_weight = true;
        const std::uint16_t joint = vertex.joints[influence];
        const auto* begin = joints_buffer.data();
        const auto* end = begin + joints_count;
        if (std::find(begin, end, joint) == end) {
            joints_buffer[joints_count++] = joint;
        }
    }
    if (has_non_zero_weight) {
        return;
    }

    const std::uint16_t fallback_joint = vertex.joints[0];
    const auto* begin = joints_buffer.data();
    const auto* end = begin + joints_count;
    if (std::find(begin, end, fallback_joint) != end) {
        return;
    }
    joints_buffer[joints_count++] = fallback_joint;
}

std::size_t count_additional_palette_joints(const MutableGpuSkinningPartition& partition,
                                            const std::array<std::uint16_t, 12U>& required_joints,
                                            std::size_t required_joint_count) {
    std::size_t additional = 0U;
    for (std::size_t i = 0U; i < required_joint_count; ++i) {
        if (partition.global_to_local_joint.find(required_joints[i]) ==
            partition.global_to_local_joint.end()) {
            ++additional;
        }
    }
    return additional;
}

bool get_or_assign_local_joint(MutableGpuSkinningPartition& partition, std::uint16_t global_joint,
                               std::size_t max_palette_joints, std::uint16_t& out_local_joint) {
    const auto existing = partition.global_to_local_joint.find(global_joint);
    if (existing != partition.global_to_local_joint.end()) {
        out_local_joint = existing->second;
        return true;
    }
    if (partition.global_joint_palette.size() >= max_palette_joints ||
        partition.global_joint_palette.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const auto new_local = static_cast<std::uint16_t>(partition.global_joint_palette.size());
    partition.global_joint_palette.push_back(global_joint);
    partition.global_to_local_joint.emplace(global_joint, new_local);
    out_local_joint = new_local;
    return true;
}

} // namespace

SkinningProgramPath choose_skinning_program_path(const SkinningProgramDecisionInputs& inputs) {
    if (!inputs.mesh_is_skinned || !inputs.has_skin_palette || !inputs.gpu_skinning_supported ||
        !inputs.skinned_program_valid) {
        return SkinningProgramPath::StaticMesh;
    }
    return SkinningProgramPath::SkinnedMesh;
}

MaterialRenderPathDecision
choose_material_render_path(const MaterialRenderPathDecisionInputs& inputs) {
    MaterialRenderPathDecision decision{};
    decision.alpha = std::clamp(inputs.base_alpha, 0.0F, 1.0F);
    decision.alpha_cutoff = std::clamp(inputs.alpha_cutoff, -1.0F, 1.0F);
    decision.alpha_cutout_enabled = decision.alpha_cutoff >= 0.0F;
    decision.use_alpha_blend_base = inputs.blend_mode == MaterialBlendMode::AlphaBlend ||
                                    (decision.alpha < 1.0F && !decision.alpha_cutout_enabled);
    return decision;
}

bool fill_skin_palette_upload_buffer(std::span<const Mat4> source_palette,
                                     std::span<Mat4> upload_buffer, std::size_t* copied_count) {
    std::fill(upload_buffer.begin(), upload_buffer.end(), Mat4::identity());

    const std::size_t copy_count = std::min(source_palette.size(), upload_buffer.size());
    std::copy_n(source_palette.begin(), copy_count, upload_buffer.begin());
    if (copied_count != nullptr) {
        *copied_count = copy_count;
    }
    return source_palette.size() > upload_buffer.size();
}

bool build_gpu_skinning_partitions(std::span<const SkinnedMeshVertex> vertices,
                                   std::span<const std::uint32_t> indices,
                                   std::size_t max_palette_joints,
                                   std::vector<GpuSkinningPartition>& out_partitions,
                                   std::string* error_message) {
    out_partitions.clear();
    if (max_palette_joints == 0U) {
        if (error_message != nullptr) {
            *error_message = "max_palette_joints must be > 0";
        }
        return false;
    }
    if ((indices.size() % 3U) != 0U) {
        if (error_message != nullptr) {
            *error_message = "index buffer must contain complete triangles";
        }
        return false;
    }
    if (indices.empty()) {
        return true;
    }
    if (vertices.empty()) {
        if (error_message != nullptr) {
            *error_message = "non-empty index buffer requires non-empty vertex buffer";
        }
        return false;
    }

    std::vector<MutableGpuSkinningPartition> mutable_partitions;
    mutable_partitions.reserve(4U);

    for (std::size_t base = 0U; base < indices.size(); base += 3U) {
        const std::array<std::uint32_t, 3U> source_indices = {
            indices[base],
            indices[base + 1U],
            indices[base + 2U],
        };
        for (std::uint32_t source_index : source_indices) {
            if (static_cast<std::size_t>(source_index) >= vertices.size()) {
                if (error_message != nullptr) {
                    *error_message = "triangle index out of range for source vertices";
                }
                return false;
            }
        }

        std::array<std::uint16_t, 12U> triangle_required_joints{};
        std::size_t triangle_required_joint_count = 0U;
        append_vertex_influence_joints(vertices[source_indices[0]], triangle_required_joints,
                                       triangle_required_joint_count);
        append_vertex_influence_joints(vertices[source_indices[1]], triangle_required_joints,
                                       triangle_required_joint_count);
        append_vertex_influence_joints(vertices[source_indices[2]], triangle_required_joints,
                                       triangle_required_joint_count);
        if (triangle_required_joint_count > max_palette_joints) {
            if (error_message != nullptr) {
                *error_message = "single triangle references more joints than palette budget";
            }
            return false;
        }

        std::size_t selected_partition_index = mutable_partitions.size();
        for (std::size_t partition_index = 0U; partition_index < mutable_partitions.size();
             ++partition_index) {
            const std::size_t additional_joints = count_additional_palette_joints(
                mutable_partitions[partition_index], triangle_required_joints,
                triangle_required_joint_count);
            const std::size_t resulting_palette_size =
                mutable_partitions[partition_index].global_joint_palette.size() + additional_joints;
            if (resulting_palette_size <= max_palette_joints) {
                selected_partition_index = partition_index;
                break;
            }
        }

        if (selected_partition_index == mutable_partitions.size()) {
            mutable_partitions.push_back(MutableGpuSkinningPartition{});
        }
        MutableGpuSkinningPartition& partition = mutable_partitions[selected_partition_index];

        for (std::uint32_t source_vertex_index : source_indices) {
            std::uint32_t local_vertex_index = 0U;
            const auto existing_vertex = partition.source_vertex_to_local.find(source_vertex_index);
            if (existing_vertex != partition.source_vertex_to_local.end()) {
                local_vertex_index = existing_vertex->second;
            } else {
                if (partition.vertices.size() >= std::numeric_limits<std::uint32_t>::max()) {
                    if (error_message != nullptr) {
                        *error_message = "partition vertex count exceeds 32-bit index range";
                    }
                    return false;
                }
                SkinnedMeshVertex remapped_vertex = vertices[source_vertex_index];
                for (std::size_t influence = 0U; influence < remapped_vertex.joints.size();
                     ++influence) {
                    if (remapped_vertex.weights[influence] <= kWeightEpsilon) {
                        remapped_vertex.joints[influence] = 0U;
                        continue;
                    }
                    std::uint16_t local_joint = 0U;
                    if (!get_or_assign_local_joint(partition, remapped_vertex.joints[influence],
                                                   max_palette_joints, local_joint)) {
                        if (error_message != nullptr) {
                            *error_message = "failed assigning local joint index for partition";
                        }
                        return false;
                    }
                    remapped_vertex.joints[influence] = local_joint;
                }
                local_vertex_index = static_cast<std::uint32_t>(partition.vertices.size());
                partition.vertices.push_back(remapped_vertex);
                partition.source_vertex_to_local.emplace(source_vertex_index, local_vertex_index);
            }
            partition.indices.push_back(local_vertex_index);
        }
    }

    out_partitions.reserve(mutable_partitions.size());
    for (MutableGpuSkinningPartition& mutable_partition : mutable_partitions) {
        out_partitions.push_back(GpuSkinningPartition{
            .vertices = std::move(mutable_partition.vertices),
            .indices = std::move(mutable_partition.indices),
            .global_joint_palette = std::move(mutable_partition.global_joint_palette),
        });
    }
    return true;
}

} // namespace isla::client
