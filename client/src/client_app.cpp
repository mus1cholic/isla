#include "client_app.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>

#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "win32_layered_overlay.hpp"

namespace isla::client {

namespace {

constexpr char kMeshAssetEnvVar[] = "ISLA_MESH_ASSET";

MeshData make_fallback_quad_mesh() {
    MeshData mesh;
    mesh.set_triangles({
        Triangle{
            .a = { .x = -0.5F, .y = 0.5F, .z = 0.0F },
            .b = { .x = -0.5F, .y = -0.5F, .z = 0.0F },
            .c = { .x = 0.5F, .y = -0.5F, .z = 0.0F },
            .uv_a = { .x = 0.0F, .y = 0.0F },
            .uv_b = { .x = 0.0F, .y = 1.0F },
            .uv_c = { .x = 1.0F, .y = 1.0F },
        },
        Triangle{
            .a = { .x = -0.5F, .y = 0.5F, .z = 0.0F },
            .b = { .x = 0.5F, .y = -0.5F, .z = 0.0F },
            .c = { .x = 0.5F, .y = 0.5F, .z = 0.0F },
            .uv_a = { .x = 0.0F, .y = 0.0F },
            .uv_b = { .x = 1.0F, .y = 1.0F },
            .uv_c = { .x = 1.0F, .y = 0.0F },
        },
    });
    return mesh;
}

} // namespace

ClientApp::ClientApp() : ClientApp(default_sdl_runtime()) {}

ClientApp::ClientApp(const ISdlRuntime& sdl_runtime) : sdl_runtime_(sdl_runtime) {}

int ClientApp::run() {
    if (!initialize()) {
        shutdown();
        return 1;
    }

    while (is_running_) {
        tick();
        render();
    }

    shutdown();
    return 0;
}

bool ClientApp::initialize() {
    if (!sdl_runtime_.init_video()) {
        LOG(ERROR) << "ClientApp: SDL video init failed: " << SDL_GetError();
        return false;
    }

    window_ = sdl_runtime_.create_window("isla_overlay", window_width_, window_height_,
                                         SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        LOG(ERROR) << "ClientApp: SDL window creation failed: " << SDL_GetError();
        return false;
    }

    sdl_runtime_.maximize_window(window_);
    if (!configure_win32_layered_overlay(window_)) {
        LOG(WARNING) << "ClientApp: layered overlay mode not applied (non-Windows or HWND lookup failure).";
    }

    int pixel_width = window_width_;
    int pixel_height = window_height_;
    if (sdl_runtime_.get_window_size_in_pixels(window_, &pixel_width, &pixel_height)) {
        window_width_ = pixel_width;
        window_height_ = pixel_height;
    }

    if (!model_renderer_.initialize(window_, renderer_, RenderSize{ window_width_, window_height_ })) {
        LOG(ERROR) << "ClientApp: model renderer initialize failed";
        return false;
    }

    load_startup_mesh();
    is_running_ = true;
    return true;
}

void ClientApp::load_startup_mesh() {
    world_.materials().push_back(Material{});

    const char* mesh_asset_path = std::getenv(kMeshAssetEnvVar);
    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        world_.meshes().push_back(make_fallback_quad_mesh());
        world_.objects().push_back(RenderObject{ .mesh_id = 0U, .material_id = 0U, .visible = true });
        LOG(INFO) << "ClientApp: no ISLA_MESH_ASSET set, using fallback quad";
        return;
    }

    mesh_asset_loader::MeshAssetLoadResult loaded = mesh_asset_loader::load_from_file(mesh_asset_path);
    if (!loaded.ok) {
        LOG(WARNING) << "ClientApp: mesh load failed for ISLA_MESH_ASSET='" << mesh_asset_path
                     << "' error='" << loaded.error_message << "'; using fallback quad";
        world_.meshes().push_back(make_fallback_quad_mesh());
        world_.objects().push_back(RenderObject{ .mesh_id = 0U, .material_id = 0U, .visible = true });
        return;
    }

    MeshData mesh;
    mesh.set_triangles(std::move(loaded.triangles));
    world_.meshes().push_back(std::move(mesh));
    world_.objects().push_back(RenderObject{ .mesh_id = 0U, .material_id = 0U, .visible = true });
    LOG(INFO) << "ClientApp: loaded mesh from ISLA_MESH_ASSET='" << mesh_asset_path << "'";
}

void ClientApp::tick() {
    SDL_Event event;
    while (sdl_runtime_.poll_event(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            is_running_ = false;
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            event.type == SDL_EVENT_WINDOW_RESIZED) {
            int width = window_width_;
            int height = window_height_;
            if (sdl_runtime_.get_window_size_in_pixels(window_, &width, &height)) {
                window_width_ = width;
                window_height_ = height;
                model_renderer_.on_resize(RenderSize{ window_width_, window_height_ });
            }
        }
    }
}

void ClientApp::render() const {
    model_renderer_.render(world_);
}

void ClientApp::shutdown() {
    model_renderer_.shutdown();

    if (renderer_ != nullptr) {
        sdl_runtime_.destroy_renderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        sdl_runtime_.destroy_window(window_);
        window_ = nullptr;
    }

    sdl_runtime_.quit();
    is_running_ = false;
}

} // namespace isla::client


