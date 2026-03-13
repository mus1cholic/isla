#pragma once

#include "absl/status/status.h"
#include "ai_gateway_client_session.hpp"
#include "client_app_internal_types.hpp"
#include "client_sdl_runtime.hpp"
#include "isla/engine/render/animated_gltf.hpp"
#include "isla/engine/render/animation_playback_controller.hpp"
#include "isla/engine/render/model_renderer.hpp"
#include "isla/engine/render/pmx_physics_sidecar.hpp"
#include "isla/engine/render/render_world.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

namespace isla::client {

namespace internal {
class ClientAppTestHooks;
}

class ClientApp {
  public:
    ClientApp();
    explicit ClientApp(const ISdlRuntime& sdl_runtime);
    int run();

  private:
    friend class internal::ClientAppTestHooks;

    bool initialize();
    void tick();
    void render() const;
    void shutdown();
    void load_startup_mesh();
    void tick_animation(float dt_seconds);
    void configure_animation_playback_from_environment();
    void populate_world_from_animated_asset();
    void load_physics_sidecar_for_asset(std::string_view asset_path);
    void append_physics_proxy_meshes();
    void tick_physics_proxies(bool recompute_bounds);
    void initialize_ai_gateway_from_environment();
    [[nodiscard]] absl::Status start_ai_gateway_session(AiGatewayClientConfig config,
                                                        std::string canned_prompt);
    void shutdown_ai_gateway();
    void drain_gateway_events();
    void process_gateway_message(const shared::ai_gateway::GatewayMessage& message);
    void process_gateway_transport_closed(const absl::Status& status);
    void enqueue_gateway_message(const shared::ai_gateway::GatewayMessage& message);
    void enqueue_gateway_transport_closed(absl::Status status);
    void send_gateway_canned_prompt();

    struct GatewayQueuedEvent {
        enum class Kind {
            Message = 0,
            TransportClosed,
        };

        Kind kind = Kind::Message;
        std::optional<shared::ai_gateway::GatewayMessage> message;
        absl::Status transport_status = absl::OkStatus();
    };

    struct GatewayState {
        bool enabled = false;
        bool connected = false;
        std::uint64_t next_turn_sequence = 1U;
        std::string canned_prompt = "hello!";
        std::optional<std::string> session_id;
        std::optional<std::string> inflight_turn_id;
        std::optional<std::string> last_reply_text;
        std::optional<std::string> last_error;
    };

    bool is_running_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int window_width_ = 1280;
    int window_height_ = 720;
    std::uint64_t last_tick_ns_ = 0U;
    RenderWorld world_{};
    ModelRenderer model_renderer_{};
    std::optional<animated_gltf::AnimatedGltfAsset> animated_asset_;
    std::optional<pmx_physics_sidecar::SidecarData> physics_sidecar_;
    animated_gltf::AnimationPlaybackController animation_playback_;
    std::vector<internal::AnimatedMeshBinding> animated_mesh_bindings_;
    std::vector<internal::PhysicsColliderBinding> physics_collider_bindings_;
    std::uint32_t animation_tick_count_ = 0U;
    bool gpu_skinning_authoritative_ = false;
    std::optional<std::size_t> physics_proxy_material_id_;
    std::unique_ptr<AiGatewayClientSession> ai_gateway_session_;
    mutable std::mutex gateway_event_mutex_;
    std::deque<GatewayQueuedEvent> gateway_event_queue_;
    GatewayState gateway_state_{};
    const ISdlRuntime& sdl_runtime_;
};

} // namespace isla::client
