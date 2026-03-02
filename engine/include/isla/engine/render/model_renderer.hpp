#pragma once

#include "isla/engine/render/renderer_backend.hpp"

#if defined(_WIN32)
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
#endif

namespace isla::client {

class ModelRenderer final : public IRendererBackend {
  public:
    bool initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) override;
    void on_resize(RenderSize size) override;
    void render(const RenderWorld& world) const override;
    void shutdown() override;

  private:
#if defined(_WIN32)
    bool create_render_target();
    void release_render_target();

    SDL_Window* window_ = nullptr;
    RenderSize render_size_{};
    ::IDXGISwapChain* swap_chain_ = nullptr;
    ::ID3D11Device* device_ = nullptr;
    ::ID3D11DeviceContext* device_context_ = nullptr;
    ::ID3D11RenderTargetView* render_target_view_ = nullptr;
#endif
};

} // namespace isla::client
