#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "isla/engine/render/render_world.hpp"

namespace isla::client::mesh_asset_loader {

struct MeshAssetMaterial {
    Color3 base_color{ .r = 1.0F, .g = 1.0F, .b = 1.0F };
    float base_alpha = 1.0F;
    float alpha_cutoff = -1.0F;
    std::string albedo_texture_path;
    MaterialBlendMode blend_mode = MaterialBlendMode::Opaque;
    // isla uses LH coordinates (+Z forward), but we load RH glTF assets (+Z forward) verbatim.
    // Because Mat4::look_at is explicit LH, the X-axis is effectively mirrored relative to the
    // camera, flipping the visual winding order. We cull CCW to correctly discard back-faces.
    MaterialCullMode cull_mode = MaterialCullMode::CounterClockwise;
};

struct MeshAssetPrimitive {
    std::vector<Triangle> triangles;
    MeshAssetMaterial material{};
    bool has_source_identity = false;
    std::size_t source_mesh_index = 0U;
    std::size_t source_primitive_index = 0U;
    std::string source_material_name;
};

struct MeshAssetLoadResult {
    bool ok = false;
    std::vector<MeshAssetPrimitive> primitives;
    std::vector<Triangle> triangles;
    MeshAssetMaterial material{};
    std::string error_message;
};

[[nodiscard]] MeshAssetLoadResult load_from_file(std::string_view asset_path);

// Resolves a texture path relative to the asset's parent directory while enforcing
// the same hardening rules used for glTF image URI resolution.
[[nodiscard]] std::string resolve_asset_relative_texture_path(std::string_view asset_path,
                                                              std::string_view texture_path);

} // namespace isla::client::mesh_asset_loader
