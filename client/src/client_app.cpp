#include "client_app.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
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
constexpr std::string_view kAiGatewayEnabledEnvVar = "ISLA_AI_GATEWAY_ENABLED";
constexpr std::string_view kAiGatewayHostEnvVar = "ISLA_AI_GATEWAY_HOST";
constexpr std::string_view kAiGatewayPortEnvVar = "ISLA_AI_GATEWAY_PORT";
constexpr std::string_view kAiGatewayPathEnvVar = "ISLA_AI_GATEWAY_PATH";
constexpr std::string_view kAiGatewayPromptEnvVar = "ISLA_AI_GATEWAY_PROMPT";
constexpr std::string_view kAiGatewaySendHotkey = "G";

enum class GatewayEnvValueSource {
    Unset = 0,
    ProcessEnv,
    DotEnv,
    DefaultValue,
};

struct GatewayEnvValue {
    std::optional<std::string> value;
    GatewayEnvValueSource source = GatewayEnvValueSource::Unset;
};

bool env_var_is_truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES" ||
           value == "on" || value == "ON";
}

const char* gateway_env_value_source_name(GatewayEnvValueSource source) {
    switch (source) {
    case GatewayEnvValueSource::Unset:
        return "unset";
    case GatewayEnvValueSource::ProcessEnv:
        return "process_env";
    case GatewayEnvValueSource::DotEnv:
        return "dotenv";
    case GatewayEnvValueSource::DefaultValue:
        return "default";
    }
    return "unknown";
}

std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0U;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool is_quoted_value(std::string_view value) {
    return value.size() >= 2U && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''));
}

std::string unquote_value(std::string_view value) {
    if (is_quoted_value(value)) {
        return std::string(value.substr(1U, value.size() - 2U));
    }
    return std::string(value);
}

using DotEnvMap = std::unordered_map<std::string, std::string>;

DotEnvMap load_dotenv_file(std::string_view path) {
    std::ifstream input{ std::string(path) };
    if (!input.is_open()) {
        return {};
    }

    DotEnvMap values;
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = trim_ascii(std::string_view(trimmed).substr(0U, equals));
        if (key.empty()) {
            continue;
        }

        std::string value = trim_ascii(std::string_view(trimmed).substr(equals + 1U));
        if (!is_quoted_value(value)) {
            if (const std::size_t comment = value.find('#'); comment != std::string::npos) {
                value = trim_ascii(std::string_view(value).substr(0U, comment));
            }
        }
        values.insert_or_assign(key, unquote_value(value));
    }

    return values;
}

std::vector<std::filesystem::path> dotenv_candidate_paths() {
    std::vector<std::filesystem::path> candidates;
    if (const char* workspace_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
        workspace_dir != nullptr && workspace_dir[0] != '\0') {
        candidates.emplace_back(std::filesystem::path(workspace_dir) / ".env");
    }

    std::error_code current_path_error;
    const std::filesystem::path current_path = std::filesystem::current_path(current_path_error);
    if (!current_path_error) {
        if (candidates.empty() || candidates.back() != (current_path / ".env")) {
            candidates.emplace_back(current_path / ".env");
        }
    }
    return candidates;
}

DotEnvMap lookup_dotenv_values() {
    for (const std::filesystem::path& candidate : dotenv_candidate_paths()) {
        const DotEnvMap loaded = load_dotenv_file(candidate.string());
        if (!loaded.empty()) {
            VLOG(1) << "ClientApp: using .env file at " << candidate.string()
                    << " for local gateway defaults";
            return loaded;
        }
    }
    return {};
}

std::optional<std::string> read_process_env_string(std::string_view name) {
    const std::string env_name(name);
    const char* value = std::getenv(env_name.c_str());
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

GatewayEnvValue read_gateway_env_value(std::string_view name, const DotEnvMap& dotenv_values) {
    if (const std::optional<std::string> process_value = read_process_env_string(name);
        process_value.has_value()) {
        return GatewayEnvValue{
            .value = process_value,
            .source = GatewayEnvValueSource::ProcessEnv,
        };
    }

    const auto it = dotenv_values.find(std::string(name));
    if (it == dotenv_values.end() || it->second.empty()) {
        return GatewayEnvValue{};
    }
    return GatewayEnvValue{
        .value = it->second,
        .source = GatewayEnvValueSource::DotEnv,
    };
}

std::optional<std::uint16_t> parse_gateway_env_port(std::string_view name,
                                                    const GatewayEnvValue& value) {
    if (!value.value.has_value()) {
        return std::nullopt;
    }

    std::uint32_t port = 0U;
    const char* begin = value.value->data();
    const char* end = value.value->data() + value.value->size();
    const auto parsed = std::from_chars(begin, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0U || port > 65535U) {
        LOG(WARNING) << "ClientApp: ignoring invalid AI gateway port in " << name << " value='"
                     << *value.value << "'";
        return std::nullopt;
    }

    return static_cast<std::uint16_t>(port);
}

std::string bool_to_enabled_label(bool value) {
    return value ? "yes" : "no";
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
    if (!sdl_runtime_.start_text_input(window_)) {
        LOG(WARNING) << "ClientApp: SDL text input start failed: " << SDL_GetError();
    }
    model_renderer_.set_debug_overlay_enabled(true);
    if (!set_win32_overlay_input_passthrough(window_, false)) {
        VLOG(1) << "ClientApp: overlay input passthrough toggle unavailable";
    }
    load_startup_mesh();
    initialize_ai_gateway_from_environment();
    update_debug_overlay();
    update_gateway_chat_panel();
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
    drain_gateway_events();
    if (const std::optional<std::string> submitted_text =
            model_renderer_.take_chat_submit_request();
        submitted_text.has_value()) {
        send_gateway_chat_message(*submitted_text);
    }

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
        model_renderer_.handle_event(event);

        if (event.type == SDL_EVENT_QUIT) {
            is_running_ = false;
            continue;
        }

        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            event.key.scancode == SDL_SCANCODE_G && !model_renderer_.wants_keyboard_capture()) {
            send_gateway_canned_prompt();
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
    drain_gateway_events();
    update_debug_overlay();
    update_gateway_chat_panel();
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

void ClientApp::render() {
    model_renderer_.render(world_);
}

void ClientApp::update_debug_overlay() {
    const std::string hud_state = [&]() -> std::string {
        if (gateway_state_.last_error.has_value()) {
            return "error";
        }
        if (gateway_state_.last_reply_text.has_value()) {
            return "reply";
        }
        if (gateway_state_.inflight_turn_id.has_value()) {
            return "inflight";
        }
        if (gateway_state_.connected) {
            return "connected";
        }
        if (gateway_state_.enabled) {
            return "disconnected";
        }
        return "disabled";
    }();
    if (hud_state != last_gateway_hud_state_) {
        VLOG(1) << "ClientApp: gateway HUD state changed state='" << hud_state << "' session_id='"
                << gateway_state_.session_id.value_or("<none>") << "' turn_id='"
                << gateway_state_.inflight_turn_id.value_or("<none>") << "'";
        last_gateway_hud_state_ = hud_state;
    }

    // NOTICE: This temporary debug HUD rebuilds its lines every frame for simplicity. If the
    // overlay becomes a longer-lived surface, prefer caching the rendered lines and regenerating
    // them only when the displayed gateway state changes.
    std::vector<std::string> overlay_lines;
    overlay_lines.reserve(7U);
    overlay_lines.emplace_back("Gateway Debug HUD");
    overlay_lines.emplace_back("Press G to send the canned prompt");
    overlay_lines.emplace_back("Enabled: " +
                               std::string(bool_to_enabled_label(gateway_state_.enabled)));
    overlay_lines.emplace_back("Connected: " +
                               std::string(bool_to_enabled_label(gateway_state_.connected)));
    overlay_lines.emplace_back("Session: " + gateway_state_.session_id.value_or("<none>"));
    overlay_lines.emplace_back("Inflight: " + gateway_state_.inflight_turn_id.value_or("<none>"));
    if (gateway_state_.last_reply_text.has_value()) {
        overlay_lines.emplace_back("Reply: " + *gateway_state_.last_reply_text);
    } else if (gateway_state_.last_error.has_value()) {
        overlay_lines.emplace_back("Error: " + *gateway_state_.last_error);
    } else {
        overlay_lines.emplace_back("Status: idle");
    }

    model_renderer_.set_debug_overlay_lines(overlay_lines);
}

void ClientApp::update_gateway_chat_panel() {
    if (!gateway_chat_panel_dirty_) {
        return;
    }

    gateway_chat_panel_state_cache_ = ChatPanelState{};
    gateway_chat_panel_state_cache_.enabled = true;
    gateway_chat_panel_state_cache_.connected = gateway_state_.connected;
    gateway_chat_panel_state_cache_.turn_in_flight = gateway_state_.inflight_turn_id.has_value();
    if (gateway_state_.last_error.has_value()) {
        gateway_chat_panel_state_cache_.status_line = "Error: " + *gateway_state_.last_error;
    } else if (gateway_chat_panel_state_cache_.turn_in_flight) {
        gateway_chat_panel_state_cache_.status_line = "Assistant is responding...";
    } else if (gateway_state_.connected) {
        gateway_chat_panel_state_cache_.status_line = "Connected";
    } else if (gateway_state_.enabled) {
        gateway_chat_panel_state_cache_.status_line = "Disconnected";
    } else {
        gateway_chat_panel_state_cache_.status_line = "Gateway disabled";
    }
    gateway_chat_panel_state_cache_.transcript.clear();
    gateway_chat_panel_state_cache_.transcript.reserve(gateway_chat_transcript_.size());
    for (const GatewayChatEntry& entry : gateway_chat_transcript_) {
        gateway_chat_panel_state_cache_.transcript.push_back(ChatPanelEntry{
            .role = entry.role,
            .text = entry.text,
        });
    }
    model_renderer_.set_chat_panel_state(gateway_chat_panel_state_cache_);
    gateway_chat_panel_dirty_ = false;
}

void ClientApp::set_assistant_transcript_entry_for_turn(std::string_view output_turn_id,
                                                        std::string_view text) {
    if (gateway_state_.inflight_turn_id.has_value() &&
        *gateway_state_.inflight_turn_id == output_turn_id &&
        gateway_inflight_assistant_entry_index_.has_value() &&
        *gateway_inflight_assistant_entry_index_ < gateway_chat_transcript_.size()) {
        gateway_chat_transcript_.at(*gateway_inflight_assistant_entry_index_).text =
            std::string(text);
        mark_gateway_chat_panel_dirty();
        return;
    }

    gateway_chat_transcript_.push_back(GatewayChatEntry{
        .role = ChatPanelEntryRole::Assistant,
        .text = std::string(text),
    });
    gateway_inflight_assistant_entry_index_ = gateway_chat_transcript_.size() - 1U;
    mark_gateway_chat_panel_dirty();
}

void ClientApp::mark_gateway_chat_panel_dirty() {
    gateway_chat_panel_dirty_ = true;
}

void ClientApp::shutdown() {
    shutdown_ai_gateway();
    model_renderer_.shutdown();
    if (window_ != nullptr) {
        sdl_runtime_.stop_text_input(window_);
    }

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

void ClientApp::initialize_ai_gateway_from_environment() {
    const DotEnvMap dotenv_values = lookup_dotenv_values();
    const GatewayEnvValue enabled = read_gateway_env_value(kAiGatewayEnabledEnvVar, dotenv_values);
    if (!enabled.value.has_value() || !env_var_is_truthy(*enabled.value)) {
        LOG(INFO) << "ClientApp: AI gateway startup skipped because " << kAiGatewayEnabledEnvVar
                  << " is not enabled"
                  << " source=" << gateway_env_value_source_name(enabled.source);
        return;
    }

    AiGatewayClientConfig config{};
    const GatewayEnvValue host = read_gateway_env_value(kAiGatewayHostEnvVar, dotenv_values);
    if (host.value.has_value()) {
        config.host = *host.value;
    }
    const GatewayEnvValue port_value = read_gateway_env_value(kAiGatewayPortEnvVar, dotenv_values);
    if (const std::optional<std::uint16_t> port =
            parse_gateway_env_port(kAiGatewayPortEnvVar, port_value);
        port.has_value()) {
        config.port = *port;
    }
    const GatewayEnvValue path = read_gateway_env_value(kAiGatewayPathEnvVar, dotenv_values);
    if (path.value.has_value()) {
        config.path = *path.value;
    }

    GatewayEnvValue prompt = read_gateway_env_value(kAiGatewayPromptEnvVar, dotenv_values);
    if (!prompt.value.has_value()) {
        prompt = GatewayEnvValue{
            .value = std::string("hello!"),
            .source = GatewayEnvValueSource::DefaultValue,
        };
    }
    const GatewayEnvValueSource host_source =
        host.value.has_value() ? host.source : GatewayEnvValueSource::DefaultValue;
    const GatewayEnvValueSource resolved_port_source =
        port_value.value.has_value() ? port_value.source : GatewayEnvValueSource::DefaultValue;
    const GatewayEnvValueSource path_source =
        path.value.has_value() ? path.source : GatewayEnvValueSource::DefaultValue;
    const std::string host_for_log = config.host;
    const std::uint16_t port_for_log = config.port;
    const std::string path_for_log = config.path;
    LOG(INFO) << "ClientApp: AI gateway startup requested host='" << config.host
              << "' host_source=" << gateway_env_value_source_name(host_source)
              << " port=" << config.port
              << " port_source=" << gateway_env_value_source_name(resolved_port_source) << " path='"
              << config.path << "' path_source=" << gateway_env_value_source_name(path_source)
              << " prompt_source=" << gateway_env_value_source_name(prompt.source);
    const absl::Status status = start_ai_gateway_session(std::move(config), *prompt.value);
    if (!status.ok()) {
        LOG(WARNING) << "ClientApp: AI gateway session unavailable: " << status << " host='"
                     << host_for_log << "' port=" << port_for_log << " path='" << path_for_log
                     << "' detail='is isla_ai_gateway running and reachable?'";
        return;
    }

    LOG(INFO) << "ClientApp: AI gateway connected; press " << kAiGatewaySendHotkey
              << " to send the canned prompt";
}

absl::Status ClientApp::start_ai_gateway_session(AiGatewayClientConfig config,
                                                 std::string canned_prompt) {
    shutdown_ai_gateway();
    {
        std::lock_guard<std::mutex> lock(gateway_event_mutex_);
        gateway_event_queue_.clear();
    }
    gateway_state_.enabled = false;
    gateway_state_.connected = false;
    gateway_state_.session_id.reset();
    gateway_state_.inflight_turn_id.reset();
    gateway_state_.last_reply_text.reset();
    gateway_state_.last_error.reset();
    gateway_chat_transcript_.clear();
    gateway_inflight_assistant_entry_index_.reset();
    mark_gateway_chat_panel_dirty();

    config.on_message = [this](const shared::ai_gateway::GatewayMessage& message) {
        enqueue_gateway_message(message);
    };
    config.on_transport_closed = [this](absl::Status status) {
        enqueue_gateway_transport_closed(std::move(status));
    };

    auto session = std::make_unique<AiGatewayClientSession>(std::move(config));
    const absl::Status status = session->ConnectAndStart();
    if (!status.ok()) {
        session->Close();
        return status;
    }

    ai_gateway_session_ = std::move(session);
    gateway_state_.enabled = true;
    gateway_state_.connected = true;
    gateway_state_.next_turn_sequence = 1U;
    gateway_state_.canned_prompt = std::move(canned_prompt);
    gateway_state_.session_id = ai_gateway_session_->session_id();
    gateway_state_.inflight_turn_id.reset();
    gateway_state_.last_reply_text.reset();
    gateway_state_.last_error.reset();
    gateway_chat_transcript_.clear();
    gateway_inflight_assistant_entry_index_.reset();
    mark_gateway_chat_panel_dirty();
    return absl::OkStatus();
}

void ClientApp::shutdown_ai_gateway() {
    if (ai_gateway_session_ == nullptr) {
        VLOG(1) << "ClientApp: AI gateway shutdown skipped because no session was created";
        return;
    }

    const std::string session_id_for_log = gateway_state_.session_id.value_or("<unknown>");
    const std::string inflight_turn_id_for_log = gateway_state_.inflight_turn_id.value_or("<none>");

    if (gateway_state_.connected && !gateway_state_.inflight_turn_id.has_value()) {
        LOG(INFO) << "ClientApp: requesting graceful AI gateway session end session_id='"
                  << session_id_for_log << "'";
        const absl::Status end_status = ai_gateway_session_->EndSession();
        if (!end_status.ok()) {
            LOG(WARNING) << "ClientApp: AI gateway session end failed session_id='"
                         << session_id_for_log << "' detail='" << end_status.message() << "'";
        } else {
            LOG(INFO) << "ClientApp: AI gateway session end request accepted session_id='"
                      << session_id_for_log << "'";
        }
    } else if (!gateway_state_.connected) {
        LOG(INFO) << "ClientApp: AI gateway shutdown closing transport without session end because"
                  << " the session is already disconnected";
    } else {
        LOG(INFO) << "ClientApp: AI gateway shutdown closing transport without session end because"
                  << " turn_id='" << inflight_turn_id_for_log << "' is still in flight";
    }

    ai_gateway_session_->Close();
    ai_gateway_session_.reset();
    gateway_state_.connected = false;
    gateway_state_.session_id.reset();
    gateway_state_.inflight_turn_id.reset();
    gateway_inflight_assistant_entry_index_.reset();
    mark_gateway_chat_panel_dirty();
}

void ClientApp::drain_gateway_events() {
    std::deque<GatewayQueuedEvent> queued_events;
    {
        std::lock_guard<std::mutex> lock(gateway_event_mutex_);
        queued_events.swap(gateway_event_queue_);
    }

    // Gateway callbacks run on the websocket I/O thread, so the SDL/main thread applies all
    // resulting state changes here before the frame continues.
    for (const GatewayQueuedEvent& event : queued_events) {
        if (event.kind == GatewayQueuedEvent::Kind::Message && event.message.has_value()) {
            process_gateway_message(*event.message);
            continue;
        }
        process_gateway_transport_closed(event.transport_status);
    }
}

void ClientApp::process_gateway_message(const shared::ai_gateway::GatewayMessage& message) {
    namespace protocol = shared::ai_gateway;

    switch (protocol::message_type(message)) {
    case protocol::MessageType::SessionStarted: {
        const auto& session_started = std::get<protocol::SessionStartedMessage>(message);
        gateway_state_.connected = true;
        gateway_state_.session_id = session_started.session_id;
        gateway_state_.last_error.reset();
        mark_gateway_chat_panel_dirty();
        LOG(INFO) << "ClientApp: AI gateway session started session_id='"
                  << session_started.session_id << "'";
        return;
    }
    case protocol::MessageType::TextOutput: {
        const auto& text_output = std::get<protocol::TextOutputMessage>(message);
        gateway_state_.last_reply_text = text_output.text;
        set_assistant_transcript_entry_for_turn(text_output.turn_id, text_output.text);
        LOG(INFO) << "ClientApp: AI gateway reply turn_id='" << text_output.turn_id << "' text='"
                  << text_output.text << "'";
        return;
    }
    case protocol::MessageType::AudioOutput: {
        const auto& audio_output = std::get<protocol::AudioOutputMessage>(message);
        LOG(INFO) << "ClientApp: AI gateway audio output received turn_id='" << audio_output.turn_id
                  << "' mime_type='" << audio_output.mime_type << "'";
        return;
    }
    case protocol::MessageType::TurnCompleted: {
        const auto& completed = std::get<protocol::TurnCompletedMessage>(message);
        if (gateway_state_.inflight_turn_id == completed.turn_id) {
            gateway_state_.inflight_turn_id.reset();
        }
        gateway_inflight_assistant_entry_index_.reset();
        mark_gateway_chat_panel_dirty();
        LOG(INFO) << "ClientApp: AI gateway turn completed turn_id='" << completed.turn_id << "'";
        return;
    }
    case protocol::MessageType::TurnCancelled: {
        const auto& cancelled = std::get<protocol::TurnCancelledMessage>(message);
        if (gateway_state_.inflight_turn_id == cancelled.turn_id) {
            gateway_state_.inflight_turn_id.reset();
        }
        gateway_inflight_assistant_entry_index_.reset();
        gateway_state_.last_error = "turn cancelled";
        gateway_chat_transcript_.push_back(GatewayChatEntry{
            .role = ChatPanelEntryRole::System,
            .text = "Turn cancelled.",
        });
        mark_gateway_chat_panel_dirty();
        LOG(INFO) << "ClientApp: AI gateway turn cancelled turn_id='" << cancelled.turn_id << "'";
        return;
    }
    case protocol::MessageType::SessionEnded: {
        const auto& ended = std::get<protocol::SessionEndedMessage>(message);
        gateway_state_.connected = false;
        gateway_state_.session_id = ended.session_id;
        gateway_state_.inflight_turn_id.reset();
        gateway_inflight_assistant_entry_index_.reset();
        mark_gateway_chat_panel_dirty();
        LOG(INFO) << "ClientApp: AI gateway session ended session_id='" << ended.session_id << "'";
        return;
    }
    case protocol::MessageType::Error: {
        const auto& error_message = std::get<protocol::ErrorMessage>(message);
        const std::string session_id_for_log = error_message.session_id.value_or("<none>");
        const std::string turn_id_for_log = error_message.turn_id.value_or("<none>");
        gateway_state_.last_error =
            "ai gateway error " + error_message.code + ": " + error_message.message;
        gateway_chat_transcript_.push_back(GatewayChatEntry{
            .role = ChatPanelEntryRole::System,
            .text = "Error: " + error_message.message,
        });
        if (error_message.turn_id.has_value() &&
            gateway_state_.inflight_turn_id == error_message.turn_id) {
            gateway_state_.inflight_turn_id.reset();
            gateway_inflight_assistant_entry_index_.reset();
        }
        mark_gateway_chat_panel_dirty();
        LOG(WARNING) << "ClientApp: AI gateway error code='" << error_message.code << "' message='"
                     << error_message.message << "' session_id='" << session_id_for_log
                     << "' turn_id='" << turn_id_for_log << "'";
        return;
    }
    case protocol::MessageType::SessionStart:
    case protocol::MessageType::SessionEnd:
    case protocol::MessageType::TranscriptSeed:
    case protocol::MessageType::TranscriptSeeded:
    case protocol::MessageType::TextInput:
    case protocol::MessageType::TurnCancel:
        return;
    }
}

void ClientApp::process_gateway_transport_closed(const absl::Status& status) {
    gateway_state_.connected = false;
    gateway_state_.session_id.reset();
    gateway_state_.inflight_turn_id.reset();
    gateway_inflight_assistant_entry_index_.reset();
    if (status.ok()) {
        gateway_chat_transcript_.push_back(GatewayChatEntry{
            .role = ChatPanelEntryRole::System,
            .text = "Transport closed.",
        });
        mark_gateway_chat_panel_dirty();
        LOG(INFO) << "ClientApp: AI gateway transport closed cleanly";
        return;
    }

    gateway_state_.last_error = std::string(status.message());
    gateway_chat_transcript_.push_back(GatewayChatEntry{
        .role = ChatPanelEntryRole::System,
        .text = "Transport closed: " + std::string(status.message()),
    });
    mark_gateway_chat_panel_dirty();
    LOG(WARNING) << "ClientApp: AI gateway transport closed: " << status;
}

void ClientApp::enqueue_gateway_message(const shared::ai_gateway::GatewayMessage& message) {
    std::lock_guard<std::mutex> lock(gateway_event_mutex_);
    gateway_event_queue_.push_back(GatewayQueuedEvent{
        .kind = GatewayQueuedEvent::Kind::Message,
        .message = message,
    });
}

void ClientApp::enqueue_gateway_transport_closed(absl::Status status) {
    std::lock_guard<std::mutex> lock(gateway_event_mutex_);
    gateway_event_queue_.push_back(GatewayQueuedEvent{
        .kind = GatewayQueuedEvent::Kind::TransportClosed,
        .transport_status = std::move(status),
    });
}

void ClientApp::send_gateway_canned_prompt() {
    send_gateway_chat_message(gateway_state_.canned_prompt);
}

void ClientApp::send_gateway_chat_message(std::string text) {
    text = trim_ascii(text);
    const std::string session_id_for_log = gateway_state_.session_id.value_or("<none>");
    if (text.empty()) {
        VLOG(1) << "ClientApp: AI gateway send skipped because the submitted chat text is empty";
        return;
    }
    if (ai_gateway_session_ == nullptr || !gateway_state_.connected) {
        gateway_state_.last_error = "ai gateway session is not connected";
        gateway_chat_transcript_.push_back(GatewayChatEntry{
            .role = ChatPanelEntryRole::System,
            .text = "Send skipped because the AI gateway session is not connected.",
        });
        mark_gateway_chat_panel_dirty();
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ClientApp: AI gateway send skipped because the session is not connected"
            << " session_id='" << session_id_for_log << "' prompt_bytes=" << text.size();
        return;
    }
    if (gateway_state_.inflight_turn_id.has_value()) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ClientApp: AI gateway send skipped while turn_id='"
            << *gateway_state_.inflight_turn_id << "' is still in flight session_id='"
            << session_id_for_log << "' prompt_bytes=" << text.size();
        return;
    }

    const std::string turn_id = "client_turn_" + std::to_string(gateway_state_.next_turn_sequence);
    ++gateway_state_.next_turn_sequence;

    gateway_chat_transcript_.push_back(GatewayChatEntry{
        .role = ChatPanelEntryRole::User,
        .text = text,
    });
    mark_gateway_chat_panel_dirty();

    const absl::Status status = ai_gateway_session_->SendTextInput(turn_id, text);
    if (!status.ok()) {
        gateway_state_.last_error = std::string(status.message());
        gateway_chat_transcript_.push_back(GatewayChatEntry{
            .role = ChatPanelEntryRole::System,
            .text = "Send failed: " + std::string(status.message()),
        });
        mark_gateway_chat_panel_dirty();
        LOG(WARNING) << "ClientApp: AI gateway send failed turn_id='" << turn_id << "' session_id='"
                     << session_id_for_log << "' prompt_bytes=" << text.size() << " detail='"
                     << status.message() << "'";
        return;
    }

    gateway_state_.inflight_turn_id = turn_id;
    gateway_inflight_assistant_entry_index_.reset();
    gateway_state_.last_error.reset();
    mark_gateway_chat_panel_dirty();
    LOG(INFO) << "ClientApp: AI gateway sent chat message turn_id='" << turn_id << "' session_id='"
              << session_id_for_log << "' prompt_bytes=" << text.size();
}

} // namespace isla::client
