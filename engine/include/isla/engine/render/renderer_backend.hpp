#pragma once

#include "isla/engine/render/render_world.hpp"

#include <span>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace isla::client {

inline constexpr int kDefaultRenderWidth = 1280;
inline constexpr int kDefaultRenderHeight = 720;

struct RenderSize {
    int width = kDefaultRenderWidth;
    int height = kDefaultRenderHeight;
};

class IRendererBackend {
  public:
    IRendererBackend() = default;
    virtual ~IRendererBackend() = default;

    IRendererBackend(const IRendererBackend&) = delete;
    IRendererBackend& operator=(const IRendererBackend&) = delete;
    IRendererBackend(IRendererBackend&&) = delete;
    IRendererBackend& operator=(IRendererBackend&&) = delete;

    virtual bool initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) = 0;
    [[nodiscard]] virtual bool uses_sdl_renderer() const = 0;
    [[nodiscard]] virtual bool has_homogeneous_depth() const = 0;
    virtual void on_resize(RenderSize size) = 0;
    virtual void render(const RenderWorld& world) const = 0;
    virtual void set_debug_overlay_enabled(bool enabled) = 0;
    virtual void set_debug_overlay_lines(std::span<const std::string> lines) = 0;
    virtual void shutdown() = 0;
};

} // namespace isla::client


