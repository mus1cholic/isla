#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "isla/engine/render/animated_gltf.hpp"
#include "isla/engine/render/model_renderer_skinning_utils.hpp"
#include "isla/engine/render/pmx_physics_sidecar.hpp"
#include "isla/engine/render/render_world.hpp"

namespace isla::client {

Transform make_visible_object_transform_for_meshes(std::span<const MeshData> meshes);
std::size_t find_clip_index_by_name(const animated_gltf::AnimatedGltfAsset& asset,
                                    const char* name);

std::vector<SkinnedMeshVertex>
make_render_skinned_vertices(const animated_gltf::SkinnedPrimitive& primitive);
std::vector<Triangle>
make_triangles_from_skinned_geometry(std::span<const SkinnedMeshVertex> vertices,
                                     std::span<const std::uint32_t> indices);
std::vector<Mat4> make_remapped_skin_palette(std::span<const Mat4> global_skin_matrices,
                                             std::span<const std::uint16_t> global_joints);

Mat4 make_collider_local_matrix(const pmx_physics_sidecar::Collider& collider);
std::vector<Triangle> make_triangles_for_collider(const pmx_physics_sidecar::Collider& collider);
void apply_matrix_to_triangles_in_place(std::span<const Triangle> source, const Mat4& matrix,
                                        std::vector<Triangle>& destination);
std::vector<std::string> collect_joint_names(const animated_gltf::AnimatedGltfAsset& asset);

} // namespace isla::client
