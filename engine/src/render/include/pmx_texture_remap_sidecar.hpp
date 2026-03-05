#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace isla::client::pmx_texture_remap_sidecar {

inline constexpr std::string_view kExpectedSchemaVersion = "1.0.0";
inline constexpr std::size_t kMaxSidecarFileSizeBytes =
    static_cast<const std::size_t>(2U * 1024U * 1024U);
inline constexpr std::size_t kMaxMappings = 16384U;
inline constexpr std::size_t kMaxStringLengthBytes = 512U;

enum class OverrideMode {
    IfMissing = 0,
    Always,
};

struct MappingTarget {
    std::optional<std::string> material_name;
    std::optional<std::size_t> mesh_index;
    std::optional<std::size_t> primitive_index;
};

struct Mapping {
    std::string id;
    MappingTarget target;
    std::string albedo_texture_path;
    std::optional<float> alpha_cutoff;
};

struct SidecarData {
    OverrideMode override_mode = OverrideMode::IfMissing;
    std::vector<Mapping> mappings;
};

struct SidecarLoadResult {
    bool ok = false;
    SidecarData sidecar;
    std::vector<std::string> warnings;
    std::string error_message;
};

[[nodiscard]] SidecarLoadResult load_from_file(std::string_view sidecar_path,
                                               std::string_view asset_path);

} // namespace isla::client::pmx_texture_remap_sidecar
