#include "client_app.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "animated_mesh_skinning.hpp"
#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "win32_layered_overlay.hpp"

namespace isla::client {

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr char kMeshAssetEnvVar[] = "ISLA_MESH_ASSET";
constexpr char kAnimatedGltfAssetEnvVar[] = "ISLA_ANIMATED_GLTF_ASSET";
constexpr char kAnimationClipEnvVar[] = "ISLA_ANIM_CLIP";
constexpr char kAnimationPlaybackModeEnvVar[] = "ISLA_ANIM_PLAYBACK_MODE";

std::size_t find_clip_index_by_name(const animated_gltf::AnimatedGltfAsset& asset,
                                    const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return 0U;
    }
    const auto it =
        std::ranges::find_if(asset.clips, [name](const auto& clip) { return clip.name == name; });
    return static_cast<std::size_t>(std::distance(asset.clips.cbegin(), it));
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
        LOG(WARNING)
            << "ClientApp: layered overlay mode not applied (non-Windows or HWND lookup failure).";
    }

    int pixel_width = window_width_;
    int pixel_height = window_height_;
    if (sdl_runtime_.get_window_size_in_pixels(window_, &pixel_width, &pixel_height)) {
        window_width_ = pixel_width;
        window_height_ = pixel_height;
    }

    if (!model_renderer_.initialize(
            window_, renderer_, RenderSize{ .width = window_width_, .height = window_height_ })) {
        LOG(ERROR) << "ClientApp: model renderer initialize failed";
        return false;
    }

    load_startup_mesh();
    last_tick_ns_ = sdl_runtime_.get_ticks_ns();
    is_running_ = true;
    return true;
}

void ClientApp::load_startup_mesh() {
    world_.materials().push_back(Material{});
    animated_asset_.reset();
    animation_playback_.clear_asset();
    animated_mesh_bindings_.clear();

    const char* animated_asset_path = std::getenv(kAnimatedGltfAssetEnvVar);
    if (animated_asset_path != nullptr && animated_asset_path[0] != '\0') {
        animated_gltf::AnimatedGltfLoadResult loaded =
            animated_gltf::load_from_file(animated_asset_path);
        if (!loaded.ok) {
            LOG(WARNING) << "ClientApp: animated glTF load failed for " << kAnimatedGltfAssetEnvVar
                         << "='" << animated_asset_path << "' error='" << loaded.error_message
                         << "'; falling back to static mesh path";
        } else {
            animated_asset_.emplace(std::move(loaded.asset));
            std::string playback_error;
            if (!animation_playback_.set_asset(&*animated_asset_, &playback_error)) {
                LOG(WARNING) << "ClientApp: animation playback setup failed for "
                             << kAnimatedGltfAssetEnvVar << "='" << animated_asset_path
                             << "' error='" << playback_error
                             << "'; falling back to static mesh path";
                animated_asset_.reset();
                animation_playback_.clear_asset();
            } else {
                configure_animation_playback_from_environment();
                populate_world_from_animated_asset();

                LOG(INFO) << "ClientApp: loaded animated glTF from " << kAnimatedGltfAssetEnvVar
                          << "='" << animated_asset_path
                          << "', clips=" << animated_asset_->clips.size()
                          << ", primitives=" << animated_asset_->primitives.size()
                          << ", playback_meshes=" << animated_mesh_bindings_.size();
                if (animated_mesh_bindings_.empty()) {
                    LOG(WARNING)
                        << "ClientApp: animated glTF loaded but produced zero playable meshes";
                }
                return;
            }
        }
    }

    const char* mesh_asset_path = std::getenv(kMeshAssetEnvVar);
    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        LOG(INFO) << "ClientApp: no ISLA_MESH_ASSET set, leaving scene empty";
        return;
    }

    mesh_asset_loader::MeshAssetLoadResult loaded =
        mesh_asset_loader::load_from_file(mesh_asset_path);
    if (!loaded.ok) {
        LOG(WARNING) << "ClientApp: mesh load failed for ISLA_MESH_ASSET='" << mesh_asset_path
                     << "' error='" << loaded.error_message << "'; leaving scene empty";
        return;
    }

    MeshData mesh;
    mesh.set_triangles(std::move(loaded.triangles));
    world_.meshes().push_back(std::move(mesh));
    world_.objects().push_back(RenderObject{ .mesh_id = 0U, .material_id = 0U, .visible = true });
    LOG(INFO) << "ClientApp: loaded mesh from ISLA_MESH_ASSET='" << mesh_asset_path << "'";
}

void ClientApp::configure_animation_playback_from_environment() {
    if (!animated_asset_.has_value()) {
        return;
    }
    std::string playback_error;
    const char* clip_name = std::getenv(kAnimationClipEnvVar);
    const std::size_t clip_index = find_clip_index_by_name(*animated_asset_, clip_name);
    if (clip_index < animated_asset_->clips.size()) {
        if (!animation_playback_.set_clip_index(clip_index, &playback_error)) {
            LOG(WARNING) << "ClientApp: failed selecting clip index " << clip_index << " error='"
                         << playback_error << "'";
        } else {
            LOG(INFO) << "ClientApp: selected animation clip index=" << clip_index << " name='"
                      << animated_asset_->clips[clip_index].name << "'";
        }
    } else if (clip_name != nullptr && clip_name[0] != '\0') {
        LOG(WARNING) << "ClientApp: requested clip '" << clip_name
                     << "' not found; defaulting to clip index 0";
    }

    const char* playback_mode = std::getenv(kAnimationPlaybackModeEnvVar);
    if (playback_mode == nullptr) {
        return;
    }
    const std::string mode_value(playback_mode);
    if (mode_value == "clamp") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Clamp);
        LOG(INFO) << "ClientApp: animation playback mode set to clamp";
    } else if (mode_value == "loop") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Loop);
        LOG(INFO) << "ClientApp: animation playback mode set to loop";
    } else {
        LOG(WARNING) << "ClientApp: unknown " << kAnimationPlaybackModeEnvVar << " value='"
                     << mode_value << "'; expected 'loop' or 'clamp'";
    }
}

void ClientApp::populate_world_from_animated_asset() {
    animated_mesh_bindings_.clear();
    if (!animated_asset_.has_value()) {
        return;
    }
    for (std::size_t primitive_index = 0U; primitive_index < animated_asset_->primitives.size();
         ++primitive_index) {
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset_->primitives[primitive_index];
        MeshData mesh;
        mesh.set_triangles(
            animated_mesh_skinning::make_triangles_from_skinned_primitive(primitive, nullptr));
        if (mesh.triangles().empty()) {
            continue;
        }
        world_.meshes().push_back(std::move(mesh));
        const std::size_t mesh_id = world_.meshes().size() - 1U;
        world_.objects().push_back(RenderObject{
            .mesh_id = mesh_id,
            .material_id = 0U,
            .visible = true,
        });
        animated_mesh_bindings_.push_back(AnimatedMeshBinding{
            .mesh_id = mesh_id,
            .primitive_index = primitive_index,
        });
    }
}

void ClientApp::tick() {
    const std::uint64_t now_ns = sdl_runtime_.get_ticks_ns();
    float dt_seconds = 0.0F;
    if (now_ns < last_tick_ns_) {
        LOG(WARNING) << "ClientApp: non-monotonic tick clock observed (now=" << now_ns
                     << ", previous=" << last_tick_ns_ << "); clamping dt to 0";
    } else {
        dt_seconds =
            static_cast<float>(now_ns - last_tick_ns_) / static_cast<float>(kNanosecondsPerSecond);
    }
    if (!std::isfinite(dt_seconds) || dt_seconds < 0.0F) {
        LOG(WARNING) << "ClientApp: invalid frame dt computed (" << dt_seconds
                     << "); clamping to 0";
        dt_seconds = 0.0F;
    }
    last_tick_ns_ = now_ns;
    world_.set_sim_time_seconds(world_.sim_time_seconds() + dt_seconds);

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
                model_renderer_.on_resize(
                    RenderSize{ .width = window_width_, .height = window_height_ });
            }
        }
    }

    tick_animation(dt_seconds);
}

void ClientApp::tick_animation(float dt_seconds) {
    if (!animated_asset_.has_value()) {
        return;
    }
    std::string playback_error;
    if (!animation_playback_.tick(dt_seconds, &playback_error)) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ClientApp: animation playback tick failed: " << playback_error;
        return;
    }
    if (!animation_playback_.has_cached_pose()) {
        return;
    }
    const std::vector<Mat4>& skin_matrices = animation_playback_.cached_pose().skin_matrices;
    for (const AnimatedMeshBinding& binding : animated_mesh_bindings_) {
        if (binding.mesh_id >= world_.meshes().size() ||
            binding.primitive_index >= animated_asset_->primitives.size()) {
            continue;
        }
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset_->primitives[binding.primitive_index];
        world_.meshes()[binding.mesh_id].set_triangles(
            animated_mesh_skinning::make_triangles_from_skinned_primitive(primitive,
                                                                          &skin_matrices));
    }
    const auto& playback_state = animation_playback_.state();
    LOG_EVERY_N_SEC(INFO, 2.0)
        << "ClientApp: animation playback tick clip_index=" << playback_state.clip_index
        << ", local_time_seconds=" << playback_state.local_time_seconds << ", mode="
        << (playback_state.playback_mode == animated_gltf::ClipPlaybackMode::Clamp ? "clamp"
                                                                                   : "loop");
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
