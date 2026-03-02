#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "isla/engine/render/render_types.hpp"

namespace isla::client::mesh_asset_loader {

struct MeshAssetLoadResult {
    bool ok = false;
    std::vector<Triangle> triangles;
    std::string error_message;
};

[[nodiscard]] MeshAssetLoadResult load_from_file(std::string_view asset_path);

} // namespace isla::client::mesh_asset_loader


