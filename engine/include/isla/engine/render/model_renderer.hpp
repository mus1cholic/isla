#pragma once

#include "isla/engine/render/renderer_backend.hpp"

#include <memory>
#include <span>
#include <string>

namespace isla::client {

namespace internal {
class ModelRendererTestHooks;
}

class ModelRenderer final : public IRendererBackend {
  public:
    class Impl;

    ModelRenderer();
    ~ModelRenderer() override;

    bool initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) override;
    [[nodiscard]] bool uses_sdl_renderer() const override;
    [[nodiscard]] bool has_homogeneous_depth() const override;
    [[nodiscard]] bool supports_gpu_skinning() const;
    void on_resize(RenderSize size) override;
    void render(const RenderWorld& world) const override;
    void set_debug_overlay_enabled(bool enabled) override;
    void set_debug_overlay_lines(std::span<const std::string> lines) override;
    void shutdown() override;

  private:
    friend class internal::ModelRendererTestHooks;

    std::unique_ptr<Impl> impl_;
};

} // namespace isla::client
