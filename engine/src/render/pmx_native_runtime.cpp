#include "isla/engine/render/pmx_native_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "include/pmx_saba_bridge.hpp"

namespace isla::client::pmx_native {
namespace {

constexpr std::string_view kBackendName = "saba";
constexpr std::string_view kBackendVersionPin = "29b8efa8b31c8e746f9a88020fb0ad9dcdcf3332";

const IngestionPolicy kIngestionPolicy = {
    .normalize_names_to_utf8 = true,
    .normalize_texture_paths_to_asset_directory = true,
    .reject_absolute_texture_paths = true,
    .reject_parent_traversal_texture_paths = true,
    .mirror_z_axis_into_isla_left_handed_space = true,
    .flip_v_texture_coordinate = true,
};

const SupportMatrix kSupportMatrix = {
    .parse_and_diagnostics_boundary = SupportStatus::Supported,
    .runtime_mesh_skeleton_material_bridge = SupportStatus::Planned,
    .bind_pose_display = SupportStatus::Planned,
    .skinning_bdef1 = SupportStatus::Planned,
    .skinning_bdef2 = SupportStatus::Planned,
    .skinning_bdef4 = SupportStatus::Planned,
    .skinning_sdef = SupportStatus::ExplicitFallback,
    .skinning_qdef = SupportStatus::ExplicitFallback,
    .baseline_albedo_alpha_cull_mapping = SupportStatus::Planned,
    .toon_sphere_edge_material_channels = SupportStatus::Deferred,
    .morph_runtime_parity = SupportStatus::Deferred,
    .vmd_playback = SupportStatus::Deferred,
    .pmx_physics = SupportStatus::Deferred,
};

std::string lower_extension(std::string_view asset_path) {
    std::string extension = std::filesystem::path(asset_path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

} // namespace

std::string_view backend_name() {
    return kBackendName;
}

std::string_view backend_version_pin() {
    return kBackendVersionPin;
}

const IngestionPolicy& ingestion_policy() {
    return kIngestionPolicy;
}

const SupportMatrix& support_matrix() {
    return kSupportMatrix;
}

const char* support_status_name(SupportStatus status) {
    switch (status) {
    case SupportStatus::Supported:
        return "supported";
    case SupportStatus::Planned:
        return "planned";
    case SupportStatus::ExplicitFallback:
        return "explicit_fallback";
    case SupportStatus::Deferred:
        return "deferred";
    }
    return "unknown";
}

const char* skinning_mode_name(SkinningMode mode) {
    switch (mode) {
    case SkinningMode::Bdef1:
        return "BDEF1";
    case SkinningMode::Bdef2:
        return "BDEF2";
    case SkinningMode::Bdef4:
        return "BDEF4";
    case SkinningMode::Sdef:
        return "SDEF";
    case SkinningMode::Qdef:
        return "QDEF";
    }
    return "unknown";
}

ProbeResult probe_file(std::string_view asset_path) {
    if (asset_path.empty()) {
        return ProbeResult{
            .ok = false,
            .summary = {},
            .infos = {},
            .warnings = {},
            .error_message = "PMX probe requires a non-empty asset path",
        };
    }

    if (lower_extension(asset_path) != ".pmx") {
        return ProbeResult{
            .ok = false,
            .summary = {},
            .infos = {},
            .warnings = {},
            .error_message = "PMX probe supports only .pmx assets",
        };
    }

    return internal::probe_with_saba(asset_path);
}

} // namespace isla::client::pmx_native
