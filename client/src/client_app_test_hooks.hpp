#pragma once

#include "client_app.hpp"
#include "engine/src/render/model_renderer_test_hooks.hpp"

#include "absl/status/status.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

    static void set_gpu_skinning_authoritative(ClientApp& app, bool enabled) {
        app.gpu_skinning_authoritative_ = enabled;
    }

    static void set_physics_sidecar(ClientApp& app, pmx_physics_sidecar::SidecarData sidecar) {
        app.physics_sidecar_ = std::move(sidecar);
    }

    static std::size_t physics_collider_binding_count(const ClientApp& app) {
        return app.physics_collider_bindings_.size();
    }

    static bool gpu_skinning_authoritative(const ClientApp& app) {
        return app.gpu_skinning_authoritative_;
    }

    static const RenderWorld& world(const ClientApp& app) {
        return app.world_;
    }

    static RenderWorld& mutable_world(ClientApp& app) {
        return app.world_;
    }

    static const animated_gltf::AnimationPlaybackController&
    animation_playback(const ClientApp& app) {
        return app.animation_playback_;
    }

    static bool has_animated_asset(const ClientApp& app) {
        return app.animated_asset_.has_value();
    }

    static absl::Status start_ai_gateway_session(ClientApp& app, AiGatewayClientConfig config,
                                                 std::string canned_prompt) {
        return app.start_ai_gateway_session(std::move(config), std::move(canned_prompt));
    }

    static void initialize_ai_gateway_from_environment(ClientApp& app) {
        app.initialize_ai_gateway_from_environment();
    }

    static void shutdown_ai_gateway(ClientApp& app) {
        app.shutdown_ai_gateway();
    }

    static bool gateway_connected(const ClientApp& app) {
        return app.gateway_state_.connected;
    }

    static std::optional<std::string> gateway_session_id(const ClientApp& app) {
        return app.gateway_state_.session_id;
    }

    static std::optional<std::string> gateway_inflight_turn_id(const ClientApp& app) {
        return app.gateway_state_.inflight_turn_id;
    }

    static std::optional<std::string> gateway_last_reply_text(const ClientApp& app) {
        return app.gateway_state_.last_reply_text;
    }

    static std::optional<std::string> gateway_last_error(const ClientApp& app) {
        return app.gateway_state_.last_error;
    }

    static void update_debug_overlay(ClientApp& app) {
        app.update_debug_overlay();
    }

    static bool debug_overlay_enabled(const ClientApp& app) {
        return ModelRendererTestHooks::debug_overlay_enabled(app.model_renderer_);
    }

    static std::vector<std::string> debug_overlay_lines(const ClientApp& app) {
        return ModelRendererTestHooks::debug_overlay_lines(app.model_renderer_);
    }
};

} // namespace isla::client::internal
