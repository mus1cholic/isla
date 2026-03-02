#include "isla/engine/render/model_renderer.hpp"

namespace isla::client {

bool ModelRenderer::initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) {
    (void)window;
    (void)renderer;
    (void)size;
    return true;
}

void ModelRenderer::on_resize(RenderSize size) {
    (void)size;
}

void ModelRenderer::render(const RenderWorld& world) const {
    (void)world;
}

void ModelRenderer::shutdown() {}

} // namespace isla::client

