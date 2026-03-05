#include "client_app_physics_proxies.hpp"

#include "absl/log/log.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "client_app_geometry_utils.hpp"

namespace isla::client {
namespace {

constexpr float kPhysicsProxyMaterialAlpha = 0.25F;
constexpr std::string_view kPhysicsProxyShaderName = "mesh";

} // namespace

void append_physics_proxy_meshes(
    const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
    const std::optional<pmx_physics_sidecar::SidecarData>& physics_sidecar,
    const animated_gltf::AnimationPlaybackController& animation_playback, RenderWorld& world,
    std::optional<std::size_t>& physics_proxy_material_id,
    std::vector<internal::PhysicsColliderBinding>& physics_collider_bindings) {
    physics_collider_bindings.clear();
    if (!animated_asset.has_value() || !physics_sidecar.has_value() ||
        physics_sidecar->colliders.empty()) {
        return;
    }

    if (!physics_proxy_material_id.has_value() ||
        *physics_proxy_material_id >= world.materials().size()) {
        Material physics_material{};
        physics_material.shader_name = std::string(kPhysicsProxyShaderName);
        physics_material.base_color = Color3{ .r = 0.2F, .g = 0.95F, .b = 0.35F };
        physics_material.base_alpha = kPhysicsProxyMaterialAlpha;
        physics_material.blend_mode = MaterialBlendMode::AlphaBlend;
        physics_material.cull_mode = MaterialCullMode::Disabled;
        world.materials().push_back(std::move(physics_material));
        physics_proxy_material_id = world.materials().size() - 1U;
    }

    std::unordered_map<std::string, std::size_t> joint_index_by_name;
    joint_index_by_name.reserve(animated_asset->skeleton.joints.size());
    for (std::size_t joint_index = 0U; joint_index < animated_asset->skeleton.joints.size();
         ++joint_index) {
        const std::string& name = animated_asset->skeleton.joints[joint_index].name;
        if (!name.empty()) {
            joint_index_by_name.emplace(name, joint_index);
        }
    }

    const std::vector<Mat4>* joint_matrices = nullptr;
    if (animation_playback.has_cached_pose()) {
        joint_matrices = &animation_playback.cached_pose().global_joint_matrices;
    }

    std::size_t created = 0U;
    std::size_t skipped_missing_bone = 0U;
    std::size_t skipped_invalid_geometry = 0U;
    for (const pmx_physics_sidecar::Collider& collider : physics_sidecar->colliders) {
        const auto joint_it = joint_index_by_name.find(collider.bone_name);
        if (joint_it == joint_index_by_name.end()) {
            LOG(WARNING) << "ClientApp: skipping collider '" << collider.id
                         << "' due to missing skeleton bone '" << collider.bone_name << "'";
            ++skipped_missing_bone;
            continue;
        }

        internal::PhysicsColliderBinding binding{};
        binding.bone_index = joint_it->second;
        binding.bone_local_collider_matrix = make_collider_local_matrix(collider);
        binding.bind_local_triangles = make_triangles_for_collider(collider);
        if (binding.bind_local_triangles.empty()) {
            ++skipped_invalid_geometry;
            continue;
        }

        std::vector<Triangle> initial_triangles;
        Mat4 world_matrix = binding.bone_local_collider_matrix;
        if (joint_matrices != nullptr && binding.bone_index < joint_matrices->size()) {
            world_matrix = multiply(joint_matrices->at(binding.bone_index), world_matrix);
        }
        apply_matrix_to_triangles_in_place(binding.bind_local_triangles, world_matrix,
                                           initial_triangles);

        MeshData mesh;
        mesh.set_triangles(std::move(initial_triangles));
        world.meshes().push_back(std::move(mesh));
        binding.mesh_id = world.meshes().size() - 1U;
        world.objects().push_back(RenderObject{
            .mesh_id = binding.mesh_id,
            .material_id = *physics_proxy_material_id,
            .visible = true,
        });
        physics_collider_bindings.push_back(std::move(binding));
        ++created;
    }
    VLOG(1) << "ClientApp: physics collider proxy build summary created=" << created
            << " skipped_missing_bone=" << skipped_missing_bone
            << " skipped_invalid_geometry=" << skipped_invalid_geometry
            << " source_colliders=" << physics_sidecar->colliders.size();
}

void tick_physics_proxies(
    const animated_gltf::AnimationPlaybackController& animation_playback, RenderWorld& world,
    std::span<const internal::PhysicsColliderBinding> physics_collider_bindings,
    bool recompute_bounds) {
    if (!animation_playback.has_cached_pose() || physics_collider_bindings.empty()) {
        return;
    }
    const std::vector<Mat4>& global_joint_matrices =
        animation_playback.cached_pose().global_joint_matrices;
    std::size_t updated = 0U;
    std::size_t skipped_invalid_binding = 0U;
    for (const internal::PhysicsColliderBinding& binding : physics_collider_bindings) {
        if (binding.bone_index >= global_joint_matrices.size() ||
            binding.mesh_id >= world.meshes().size()) {
            ++skipped_invalid_binding;
            continue;
        }
        MeshData& mesh = world.meshes()[binding.mesh_id];
        const Mat4 world_matrix =
            multiply(global_joint_matrices[binding.bone_index], binding.bone_local_collider_matrix);
        mesh.edit_triangles_without_recompute_bounds([&](MeshData::TriangleList& triangles) {
            apply_matrix_to_triangles_in_place(binding.bind_local_triangles, world_matrix,
                                               triangles);
        });
        if (recompute_bounds) {
            mesh.recompute_bounds();
        }
        ++updated;
    }
    if (skipped_invalid_binding > 0U) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ClientApp: physics proxy tick skipped invalid bindings count="
            << skipped_invalid_binding << " total_bindings=" << physics_collider_bindings.size()
            << " global_joint_matrices=" << global_joint_matrices.size()
            << " world_meshes=" << world.meshes().size();
    }
    VLOG_EVERY_N_SEC(1, 2.0) << "ClientApp: physics proxy tick updated=" << updated
                             << " skipped_invalid_binding=" << skipped_invalid_binding
                             << " total_bindings=" << physics_collider_bindings.size();
}

} // namespace isla::client
