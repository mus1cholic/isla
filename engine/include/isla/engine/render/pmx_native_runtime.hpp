#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace isla::client::pmx_native {

enum class SupportStatus {
    Supported = 0,
    Planned,
    ExplicitFallback,
    Deferred,
};

enum class SkinningMode {
    Bdef1 = 0,
    Bdef2,
    Bdef4,
    Sdef,
    Qdef,
};

struct IngestionPolicy {
    bool normalize_names_to_utf8 = true;
    bool normalize_texture_paths_to_asset_directory = true;
    bool reject_absolute_texture_paths = true;
    bool reject_parent_traversal_texture_paths = true;
    bool mirror_z_axis_into_isla_left_handed_space = true;
    bool flip_v_texture_coordinate = true;
};

// Phase 0 documents the PMX-native contract before runtime world population lands.
// The parser/probe boundary is available now; bind-pose rendering and deformation remain
// intentionally deferred to the later phases described in the phased plan.
struct SupportMatrix {
    SupportStatus parse_and_diagnostics_boundary = SupportStatus::Supported;
    SupportStatus runtime_mesh_skeleton_material_bridge = SupportStatus::Planned;
    SupportStatus bind_pose_display = SupportStatus::Planned;
    SupportStatus skinning_bdef1 = SupportStatus::Planned;
    SupportStatus skinning_bdef2 = SupportStatus::Planned;
    SupportStatus skinning_bdef4 = SupportStatus::Planned;
    SupportStatus skinning_sdef = SupportStatus::ExplicitFallback;
    SupportStatus skinning_qdef = SupportStatus::ExplicitFallback;
    SupportStatus baseline_albedo_alpha_cull_mapping = SupportStatus::Planned;
    SupportStatus toon_sphere_edge_material_channels = SupportStatus::Deferred;
    SupportStatus morph_runtime_parity = SupportStatus::Deferred;
    SupportStatus vmd_playback = SupportStatus::Deferred;
    SupportStatus pmx_physics = SupportStatus::Deferred;
};

struct ProbeSummary {
    std::string model_name;
    std::size_t vertex_count = 0U;
    std::size_t face_count = 0U;
    std::size_t material_count = 0U;
    std::size_t texture_count = 0U;
    std::size_t bone_count = 0U;
    std::size_t morph_count = 0U;
    std::size_t rigidbody_count = 0U;
    std::size_t joint_count = 0U;
    std::size_t softbody_count = 0U;
    std::size_t ik_bone_count = 0U;
    std::size_t append_transform_bone_count = 0U;
    std::size_t bdef1_vertex_count = 0U;
    std::size_t bdef2_vertex_count = 0U;
    std::size_t bdef4_vertex_count = 0U;
    std::size_t sdef_vertex_count = 0U;
    std::size_t qdef_vertex_count = 0U;
    std::size_t absolute_texture_reference_count = 0U;
    std::size_t parent_traversal_texture_reference_count = 0U;
    std::size_t missing_texture_reference_count = 0U;
    std::size_t sphere_texture_material_count = 0U;
    std::size_t toon_texture_material_count = 0U;
    std::size_t edge_enabled_material_count = 0U;
};

struct ProbeResult {
    bool ok = false;
    ProbeSummary summary;
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
    std::string error_message;
};

[[nodiscard]] std::string_view backend_name();
[[nodiscard]] std::string_view backend_version_pin();
[[nodiscard]] const IngestionPolicy& ingestion_policy();
[[nodiscard]] const SupportMatrix& support_matrix();
[[nodiscard]] const char* support_status_name(SupportStatus status);
[[nodiscard]] const char* skinning_mode_name(SkinningMode mode);
[[nodiscard]] ProbeResult probe_file(std::string_view asset_path);

} // namespace isla::client::pmx_native
