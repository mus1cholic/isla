#pragma once

#include "client_app.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace isla::client::internal {

class ClientAppTestHooks {
  public:
    static void load_startup_mesh(ClientApp& app) {
        app.load_startup_mesh();
    }

    static void tick(ClientApp& app) {
        app.tick();
    }

    static void tick_animation(ClientApp& app, float dt_seconds) {
        app.tick_animation(dt_seconds);
    }

    static void set_last_tick_ns(ClientApp& app, std::uint64_t tick_ns) {
        app.last_tick_ns_ = tick_ns;
    }

    static void set_animated_asset(ClientApp& app, animated_gltf::AnimatedGltfAsset asset) {
        app.animated_asset_ = std::move(asset);
        std::string error;
        (void)app.animation_playback_.set_asset(&*app.animated_asset_, &error);
    }

    static void configure_animation_playback_from_environment(ClientApp& app) {
        app.configure_animation_playback_from_environment();
    }

    static void populate_world_from_animated_asset(ClientApp& app) {
        app.populate_world_from_animated_asset();
    }

    static const RenderWorld& world(const ClientApp& app) {
        return app.world_;
    }

    static const animated_gltf::AnimationPlaybackController&
    animation_playback(const ClientApp& app) {
        return app.animation_playback_;
    }

    static bool has_animated_asset(const ClientApp& app) {
        return app.animated_asset_.has_value();
    }
};

} // namespace isla::client::internal
