#pragma once

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
    // isla uses LH coordinates, but we load RH glTF assets verbatim.
    // This mirrors the geometry along the Z-axis relative to the camera, flipping the winding order
    // on-screen. We cull CCW to properly discard the back-faces of RH assets in our LH engine.
    MaterialCullMode cull_mode = MaterialCullMode::CounterClockwise;
};

struct MeshAssetPrimitive {
    std::vector<Triangle> triangles;
    MeshAssetMaterial material{};
};

struct MeshAssetLoadResult {
    bool ok = false;
    std::vector<MeshAssetPrimitive> primitives;
    std::vector<Triangle> triangles;
    MeshAssetMaterial material{};
    std::string error_message;
};

[[nodiscard]] MeshAssetLoadResult load_from_file(std::string_view asset_path);

} // namespace isla::client::mesh_asset_loader
