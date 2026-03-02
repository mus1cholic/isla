#include "isla/engine/render/model_renderer.hpp"
#include "isla/engine/render/overlay_transparency.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <d3d11.h>
#include <windows.h>
#endif

namespace isla::client {

#if defined(_WIN32)

namespace {

HWND get_hwnd_from_sdl_window(SDL_Window* window) {
    if (window == nullptr) {
        return nullptr;
    }

    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    return static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

template <typename T> void release_com(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

#endif

bool ModelRenderer::initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) {
    (void)renderer;

#if defined(_WIN32)
    window_ = window;
    render_size_ = size;

    HWND hwnd = get_hwnd_from_sdl_window(window_);
    if (hwnd == nullptr) {
        LOG(ERROR) << "ModelRenderer: SDL window does not expose a Win32 HWND";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    swap_chain_desc.BufferDesc.Width = static_cast<UINT>(render_size_.width);
    swap_chain_desc.BufferDesc.Height = static_cast<UINT>(render_size_.height);
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.OutputWindow = hwnd;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const UINT device_flags = 0;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags, nullptr, 0, D3D11_SDK_VERSION,
        &swap_chain_desc, &swap_chain_, &device_, &feature_level, &device_context_);
    if (FAILED(hr)) {
        LOG(ERROR) << "ModelRenderer: D3D11CreateDeviceAndSwapChain failed, hr="
                   << static_cast<unsigned long>(hr);
        return false;
    }

    if (!create_render_target()) {
        shutdown();
        return false;
    }
#else
    (void)window;
    (void)size;
#endif

    return true;
}

void ModelRenderer::on_resize(RenderSize size) {
#if defined(_WIN32)
    render_size_ = size;
    if (swap_chain_ == nullptr || device_context_ == nullptr) {
        return;
    }

    if (render_size_.width <= 0 || render_size_.height <= 0) {
        return;
    }

    release_render_target();
    const HRESULT hr =
        swap_chain_->ResizeBuffers(0, static_cast<UINT>(render_size_.width),
                                   static_cast<UINT>(render_size_.height), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LOG(ERROR) << "ModelRenderer: ResizeBuffers failed, hr=" << static_cast<unsigned long>(hr);
        return;
    }

    if (!create_render_target()) {
        LOG(ERROR) << "ModelRenderer: failed to recreate render target after resize";
    }
#else
    (void)size;
#endif
}

void ModelRenderer::render(const RenderWorld& world) const {
    (void)world;
#if defined(_WIN32)
    if (device_context_ == nullptr || render_target_view_ == nullptr || swap_chain_ == nullptr) {
        return;
    }

    device_context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
    device_context_->ClearRenderTargetView(render_target_view_,
                                           OverlayTransparencyConfig::kClearColor.data());
    swap_chain_->Present(1, 0);
#endif
}

void ModelRenderer::shutdown() {
#if defined(_WIN32)
    release_render_target();
    release_com(device_context_);
    release_com(device_);
    release_com(swap_chain_);
    window_ = nullptr;
#endif
}

#if defined(_WIN32)

bool ModelRenderer::create_render_target() {
    if (swap_chain_ == nullptr || device_ == nullptr) {
        return false;
    }

    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT buffer_hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(buffer_hr) || back_buffer == nullptr) {
        LOG(ERROR) << "ModelRenderer: swap_chain->GetBuffer failed, hr="
                   << static_cast<unsigned long>(buffer_hr);
        return false;
    }

    const HRESULT rtv_hr =
        device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
    back_buffer->Release();
    if (FAILED(rtv_hr) || render_target_view_ == nullptr) {
        LOG(ERROR) << "ModelRenderer: CreateRenderTargetView failed, hr="
                   << static_cast<unsigned long>(rtv_hr);
        return false;
    }

    return true;
}

void ModelRenderer::release_render_target() {
    release_com(render_target_view_);
}

#endif

} // namespace isla::client
