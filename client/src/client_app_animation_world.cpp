#include "client_app_animation_world.hpp"

#include "absl/log/log.h"

#include <string>
#include <vector>

#include "animated_mesh_skinning.hpp"
#include "client_app_geometry_utils.hpp"
#include "isla/engine/render/model_renderer_skinning_utils.hpp"

namespace isla::client {

void populate_world_from_animated_asset(
    const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
    const animated_gltf::AnimationPlaybackController& animation_playback,
    bool gpu_skinning_authoritative, RenderWorld& world,
    std::vector<internal::AnimatedMeshBinding>& animated_mesh_bindings,
    std::uint32_t& animation_tick_count) {
    animated_mesh_bindings.clear();
    animation_tick_count = 0U;
    world.meshes().clear();
    world.objects().clear();
    if (!animated_asset.has_value()) {
        return;
    }
    for (std::size_t primitive_index = 0U; primitive_index < animated_asset->primitives.size();
         ++primitive_index) {
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset->primitives[primitive_index];
        if (gpu_skinning_authoritative) {
            std::vector<GpuSkinningPartition> partitions;
            std::string partition_error;
            const std::vector<SkinnedMeshVertex> render_vertices =
                make_render_skinned_vertices(primitive);
            if (!build_gpu_skinning_partitions(render_vertices, primitive.indices,
                                               kMaxGpuSkinningJoints, partitions,
                                               &partition_error)) {
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "ClientApp: failed to partition skinned primitive " << primitive_index
                    << " (source_vertices=" << render_vertices.size()
                    << ", source_indices=" << primitive.indices.size() << ")"
                    << " for GPU palette budget " << kMaxGpuSkinningJoints
                    << "; skipping primitive. error='" << partition_error << "'";
                continue;
            }
            if (partitions.empty()) {
                continue;
            }
            if (partitions.size() > 1U) {
                std::string palette_sizes;
                palette_sizes.reserve(partitions.size() * 4U);
                for (std::size_t partition_index = 0U; partition_index < partitions.size();
                     ++partition_index) {
                    if (!palette_sizes.empty()) {
                        palette_sizes += ",";
                    }
                    palette_sizes +=
                        std::to_string(partitions[partition_index].global_joint_palette.size());
                }
                VLOG(1) << "ClientApp: primitive " << primitive_index << " split into "
                        << partitions.size() << " GPU skinning partitions (palette_sizes=["
                        << palette_sizes << "])";
            }

            for (GpuSkinningPartition& partition : partitions) {
                internal::AnimatedMeshBinding binding;
                binding.primitive_index = primitive_index;
                binding.gpu_palette_global_joints = std::move(partition.global_joint_palette);
                if (binding.gpu_palette_global_joints.empty()) {
                    LOG_EVERY_N_SEC(WARNING, 2.0)
                        << "ClientApp: GPU partition has empty remapped joint palette "
                           "(primitive_index="
                        << primitive_index << ")";
                }

                MeshData mesh;
                mesh.set_triangles(
                    make_triangles_from_skinned_geometry(partition.vertices, partition.indices));
                mesh.set_skinned_geometry(std::move(partition.vertices),
                                          std::move(partition.indices));
                if (animation_playback.has_cached_pose()) {
                    mesh.set_skin_palette(
                        make_remapped_skin_palette(animation_playback.cached_pose().skin_matrices,
                                                   binding.gpu_palette_global_joints));
                }
                if (mesh.triangles().empty()) {
                    continue;
                }
                world.meshes().push_back(std::move(mesh));
                binding.mesh_id = world.meshes().size() - 1U;
                world.objects().push_back(RenderObject{
                    .mesh_id = binding.mesh_id,
                    .material_id = 0U,
                    .visible = true,
                });
                animated_mesh_bindings.push_back(std::move(binding));
            }
            continue;
        }

        internal::AnimatedMeshBinding binding;
        binding.primitive_index = primitive_index;
        std::vector<Triangle> initial_triangles =
            animated_mesh_skinning::make_initial_triangles_and_workspace(
                primitive, binding.skinning_workspace.get());
        MeshData mesh;
        mesh.set_triangles(std::move(initial_triangles));
        mesh.clear_skinned_geometry();
        if (mesh.triangles().empty()) {
            continue;
        }
        world.meshes().push_back(std::move(mesh));
        binding.mesh_id = world.meshes().size() - 1U;
        world.objects().push_back(RenderObject{
            .mesh_id = binding.mesh_id,
            .material_id = 0U,
            .visible = true,
        });
        animated_mesh_bindings.push_back(std::move(binding));
    }
}

void tick_animated_meshes(const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
                          const animated_gltf::AnimationPlaybackController& animation_playback,
                          bool gpu_skinning_authoritative, RenderWorld& world,
                          std::vector<internal::AnimatedMeshBinding>& animated_mesh_bindings,
                          bool should_recompute_bounds, std::size_t& recomputed_bounds_mesh_count) {
    recomputed_bounds_mesh_count = 0U;
    if (!animated_asset.has_value() || !animation_playback.has_cached_pose()) {
        return;
    }
    const std::vector<Mat4>& skin_matrices = animation_playback.cached_pose().skin_matrices;
    if (gpu_skinning_authoritative) {
        for (internal::AnimatedMeshBinding& binding : animated_mesh_bindings) {
            if (binding.mesh_id >= world.meshes().size()) {
                continue;
            }
            MeshData& mesh = world.meshes()[binding.mesh_id];
            if (binding.gpu_palette_global_joints.empty()) {
                mesh.set_skin_palette(skin_matrices);
                continue;
            }
            mesh.set_skin_palette(
                make_remapped_skin_palette(skin_matrices, binding.gpu_palette_global_joints));
        }
        return;
    }
    for (internal::AnimatedMeshBinding& binding : animated_mesh_bindings) {
        if (binding.mesh_id >= world.meshes().size() ||
            binding.primitive_index >= animated_asset->primitives.size()) {
            continue;
        }
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset->primitives[binding.primitive_index];
        MeshData& mesh = world.meshes()[binding.mesh_id];
        mesh.edit_triangles_without_recompute_bounds([&](MeshData::TriangleList& triangles) {
            animated_mesh_skinning::skin_primitive_in_place(
                primitive, &skin_matrices, binding.skinning_workspace.get(), &triangles);
        });
        if (should_recompute_bounds) {
            mesh.recompute_bounds();
            ++recomputed_bounds_mesh_count;
        }
    }
}

} // namespace isla::client
