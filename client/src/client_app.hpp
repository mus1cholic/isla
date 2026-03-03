#pragma once

#include "client_sdl_runtime.hpp"
#include "engine/src/render/include/animated_gltf.hpp"
#include "engine/src/render/include/animation_playback_controller.hpp"
#include "isla/engine/render/model_renderer.hpp"
#include "isla/engine/render/render_world.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include "animated_mesh_skinning.hpp"

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

    struct AnimatedMeshBinding {
        std::size_t mesh_id = 0U;
        std::size_t primitive_index = 0U;
        animated_mesh_skinning::PrimitiveSkinningWorkspace skinning_workspace;
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
    animated_gltf::AnimationPlaybackController animation_playback_;
    std::vector<AnimatedMeshBinding> animated_mesh_bindings_;
    std::uint32_t animation_tick_count_ = 0U;
    const ISdlRuntime& sdl_runtime_;
};

} // namespace isla::client
