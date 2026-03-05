#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "client_app_internal_types.hpp"
#include "isla/engine/render/animated_gltf.hpp"
#include "isla/engine/render/animation_playback_controller.hpp"
#include "isla/engine/render/pmx_physics_sidecar.hpp"
#include "isla/engine/render/render_world.hpp"

namespace isla::client {

void append_physics_proxy_meshes(
    const std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset,
    const std::optional<pmx_physics_sidecar::SidecarData>& physics_sidecar,
    const animated_gltf::AnimationPlaybackController& animation_playback, RenderWorld& world,
    std::optional<std::size_t>& physics_proxy_material_id,
    std::vector<internal::PhysicsColliderBinding>& physics_collider_bindings);

void tick_physics_proxies(
    const animated_gltf::AnimationPlaybackController& animation_playback, RenderWorld& world,
    std::span<const internal::PhysicsColliderBinding> physics_collider_bindings,
    bool recompute_bounds);

} // namespace isla::client
