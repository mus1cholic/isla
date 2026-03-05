#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "client_app_internal_types.hpp"
#include "isla/engine/render/animated_gltf.hpp"
#include "isla/engine/render/animation_playback_controller.hpp"
#include "isla/engine/render/pmx_physics_sidecar.hpp"
#include "isla/engine/render/render_world.hpp"

namespace isla::client {

struct StartupLoaderContext {
    RenderWorld& world;
    std::optional<animated_gltf::AnimatedGltfAsset>& animated_asset;
    std::optional<pmx_physics_sidecar::SidecarData>& physics_sidecar;
    animated_gltf::AnimationPlaybackController& animation_playback;
    std::vector<internal::AnimatedMeshBinding>& animated_mesh_bindings;
    std::vector<internal::PhysicsColliderBinding>& physics_collider_bindings;
    std::optional<std::size_t>& physics_proxy_material_id;
    std::uint32_t& animation_tick_count;
    bool gpu_skinning_authoritative = false;
    std::function<void()> configure_animation_playback_from_environment;
    std::function<void(std::string_view)> load_physics_sidecar_for_asset;
    std::function<void()> populate_world_from_animated_asset;
};

void load_startup_mesh(StartupLoaderContext& context);

} // namespace isla::client
