#pragma once

#include "client_sdl_runtime.hpp"
#include "isla/engine/render/model_renderer.hpp"
#include "isla/engine/render/render_world.hpp"

struct SDL_Window;
struct SDL_Renderer;

namespace isla::client {

class ClientApp {
  public:
    ClientApp();
    explicit ClientApp(const ISdlRuntime& sdl_runtime);
    int run();

  private:
    bool initialize();
    void tick();
    void render() const;
    void shutdown();
    void load_startup_mesh();

    bool is_running_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int window_width_ = 1280;
    int window_height_ = 720;
    RenderWorld world_{};
    ModelRenderer model_renderer_{};
    const ISdlRuntime& sdl_runtime_;
};

} // namespace isla::client


