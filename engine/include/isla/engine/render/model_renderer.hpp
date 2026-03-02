#pragma once

#include "isla/engine/render/renderer_backend.hpp"

namespace isla::client {

class ModelRenderer final : public IRendererBackend {
  public:
    bool initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) override;
    void on_resize(RenderSize size) override;
    void render(const RenderWorld& world) const override;
    void shutdown() override;
};

} // namespace isla::client

