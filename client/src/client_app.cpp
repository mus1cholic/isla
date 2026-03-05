#include "client_app.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "client_app_animation_world.hpp"
#include "client_app_geometry_utils.hpp"
#include "client_app_physics_proxies.hpp"
#include "client_app_startup_loader.hpp"
#include "win32_layered_overlay.hpp"

namespace isla::client {

namespace {

// TODO(isla): Refactor ClientApp to a pImpl-style design (ClientApp::Impl) so client_app.hpp
// can hide internal state and implementation dependencies.
constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint32_t kAnimatedBoundsRecomputeIntervalTicks = 30U;
constexpr std::string_view kAnimationClipEnvVar = "ISLA_ANIM_CLIP";
constexpr std::string_view kAnimationPlaybackModeEnvVar = "ISLA_ANIM_PLAYBACK_MODE";

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
                                         SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE |
                                             SDL_WINDOW_TRANSPARENT);
    if (window_ == nullptr) {
        LOG(ERROR) << "ClientApp: SDL window creation failed: " << SDL_GetError();
        return false;
    }

    sdl_runtime_.maximize_window(window_);
    if (!configure_win32_alpha_composited_overlay(window_)) {
        LOG(WARNING) << "ClientApp: alpha-composited overlay mode not applied (non-Windows, DWM "
                        "unavailable, or HWND lookup failure).";
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
    gpu_skinning_authoritative_ = model_renderer_.supports_gpu_skinning();
    VLOG(1) << "ClientApp: GPU skinning authoritative mode "
            << (gpu_skinning_authoritative_ ? "enabled" : "disabled")
            << " (renderer support check)";

    StartupLoaderContext context{
        .world = world_,
        .animated_asset = animated_asset_,
        .physics_sidecar = physics_sidecar_,
        .animation_playback = animation_playback_,
        .animated_mesh_bindings = animated_mesh_bindings_,
        .physics_collider_bindings = physics_collider_bindings_,
        .physics_proxy_material_id = physics_proxy_material_id_,
        .animation_tick_count = animation_tick_count_,
        .gpu_skinning_authoritative = gpu_skinning_authoritative_,
        .configure_animation_playback_from_environment =
            [this]() { configure_animation_playback_from_environment(); },
        .load_physics_sidecar_for_asset =
            [this](std::string_view asset_path) { load_physics_sidecar_for_asset(asset_path); },
        .populate_world_from_animated_asset = [this]() { populate_world_from_animated_asset(); },
    };
    isla::client::load_startup_mesh(context);
}

void ClientApp::configure_animation_playback_from_environment() {
    if (!animated_asset_.has_value()) {
        return;
    }
    std::string playback_error;
    const char* clip_name = std::getenv(kAnimationClipEnvVar.data());
    const std::size_t clip_index = find_clip_index_by_name(*animated_asset_, clip_name);
    if (clip_index < animated_asset_->clips.size()) {
        if (!animation_playback_.set_clip_index(clip_index, &playback_error)) {
            LOG(WARNING) << "ClientApp: failed selecting clip index " << clip_index << " error='"
                         << playback_error << "'";
        } else {
            VLOG(1) << "ClientApp: selected animation clip index=" << clip_index << " name='"
                    << animated_asset_->clips[clip_index].name << "'";
        }
    } else if (clip_name != nullptr && clip_name[0] != '\0') {
        LOG(WARNING) << "ClientApp: requested clip '" << clip_name
                     << "' not found; defaulting to clip index 0";
    }

    const char* playback_mode = std::getenv(kAnimationPlaybackModeEnvVar.data());
    if (playback_mode == nullptr) {
        return;
    }
    const std::string mode_value(playback_mode);
    if (mode_value == "clamp") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Clamp);
        VLOG(1) << "ClientApp: animation playback mode set to clamp";
    } else if (mode_value == "loop") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Loop);
        VLOG(1) << "ClientApp: animation playback mode set to loop";
    } else {
        LOG(WARNING) << "ClientApp: unknown " << kAnimationPlaybackModeEnvVar << " value='"
                     << mode_value << "'; expected 'loop' or 'clamp'";
    }
}

void ClientApp::load_physics_sidecar_for_asset(std::string_view asset_path) {
    physics_sidecar_.reset();
    physics_collider_bindings_.clear();
    physics_proxy_material_id_.reset();
    if (!animated_asset_.has_value()) {
        return;
    }

    std::filesystem::path sidecar_path = std::filesystem::path(asset_path);
    sidecar_path.replace_extension(".physics.json");
    std::error_code exists_error;
    const bool sidecar_exists = std::filesystem::exists(sidecar_path, exists_error);
    if (exists_error) {
        LOG(WARNING) << "ClientApp: failed checking physics sidecar path '" << sidecar_path.string()
                     << "': " << exists_error.message()
                     << "; skipping Phase 5 collider proxy import";
        return;
    }
    if (!sidecar_exists) {
        VLOG(1) << "ClientApp: no physics sidecar found at '" << sidecar_path.string()
                << "'; skipping Phase 5 collider proxy import";
        return;
    }

    const std::vector<std::string> joint_names = collect_joint_names(*animated_asset_);
    const pmx_physics_sidecar::SidecarLoadResult loaded =
        pmx_physics_sidecar::load_from_file(sidecar_path.string(), joint_names);
    for (const std::string& warning : loaded.warnings) {
        LOG(WARNING) << "ClientApp: physics sidecar warning: " << warning;
    }
    if (!loaded.ok) {
        LOG(WARNING) << "ClientApp: failed to load physics sidecar '" << sidecar_path.string()
                     << "': " << loaded.error_message;
        return;
    }
    physics_sidecar_ = loaded.sidecar;
    VLOG(1) << "ClientApp: loaded physics sidecar '" << sidecar_path.string()
            << "' colliders=" << physics_sidecar_->colliders.size()
            << " constraints=" << physics_sidecar_->constraints.size()
            << " layers=" << physics_sidecar_->collision_layers.size();
}

void ClientApp::populate_world_from_animated_asset() {
    isla::client::populate_world_from_animated_asset(
        animated_asset_, animation_playback_, gpu_skinning_authoritative_, world_,
        animated_mesh_bindings_, animation_tick_count_);
    append_physics_proxy_meshes();
    VLOG(1) << "ClientApp: animated mesh population complete, bindings="
            << animated_mesh_bindings_.size()
            << ", physics_colliders=" << physics_collider_bindings_.size()
            << ", gpu_skinning_authoritative=" << (gpu_skinning_authoritative_ ? "true" : "false");
}

void ClientApp::append_physics_proxy_meshes() {
    isla::client::append_physics_proxy_meshes(
        animated_asset_, physics_sidecar_, animation_playback_, world_, physics_proxy_material_id_,
        physics_collider_bindings_);
}

void ClientApp::tick_physics_proxies(bool recompute_bounds) {
    isla::client::tick_physics_proxies(animation_playback_, world_, physics_collider_bindings_,
                                       recompute_bounds);
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
                if (!refresh_win32_alpha_composited_overlay(window_)) {
                    LOG_EVERY_N_SEC(WARNING, 2.0)
                        << "ClientApp: failed to refresh alpha-composited overlay after window "
                           "resize/reconfigure";
                }
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

    ++animation_tick_count_;
    const bool should_recompute_bounds =
        (animation_tick_count_ % kAnimatedBoundsRecomputeIntervalTicks) == 0U;
    const auto& playback_state = animation_playback_.state();
    VLOG_EVERY_N_SEC(1, 2.0)
        << "ClientApp: animation playback tick clip_index=" << playback_state.clip_index
        << ", local_time_seconds=" << playback_state.local_time_seconds << ", mode="
        << (playback_state.playback_mode == animated_gltf::ClipPlaybackMode::Clamp ? "clamp"
                                                                                   : "loop");

    std::size_t recomputed_bounds_mesh_count = 0U;
    tick_animated_meshes(animated_asset_, animation_playback_, gpu_skinning_authoritative_, world_,
                         animated_mesh_bindings_, should_recompute_bounds,
                         recomputed_bounds_mesh_count);

    if (!gpu_skinning_authoritative_ && should_recompute_bounds &&
        recomputed_bounds_mesh_count > 0U) {
        VLOG(1) << "ClientApp: deferred animated mesh bounds recomputed for "
                << recomputed_bounds_mesh_count
                << " mesh(es) at animation_tick_count=" << animation_tick_count_;
    }
    tick_physics_proxies(should_recompute_bounds);
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
