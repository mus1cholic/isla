#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "client_app_internal_types.hpp"
#include "isla/engine/render/animated_gltf.hpp"
#include "isla/engine/render/animation_playback_controller.hpp"
#include "isla/engine/render/render_world.hpp"

namespace isla::client {

void populate_world_from_animated_asset(
    const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
    const animated_gltf::AnimationPlaybackController& animation_playback,
    bool gpu_skinning_authoritative, RenderWorld& world,
    std::vector<internal::AnimatedMeshBinding>& animated_mesh_bindings,
    std::uint32_t& animation_tick_count);

void tick_animated_meshes(const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
                          const animated_gltf::AnimationPlaybackController& animation_playback,
                          bool gpu_skinning_authoritative, RenderWorld& world,
                          std::vector<internal::AnimatedMeshBinding>& animated_mesh_bindings,
                          bool should_recompute_bounds, std::size_t& recomputed_bounds_mesh_count);

} // namespace isla::client
