#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "isla/engine/render/mesh_asset_loader.hpp"

namespace isla::client {

struct StaticTextureRemapApplicationResult {
    bool sidecar_load_failed = false;
    std::vector<bool> applied_from_texturemap;
    std::vector<std::string> warnings;
    std::vector<std::string> infos;
    std::size_t mappings_total = 0U;
    std::size_t mappings_applied = 0U;
    std::size_t mappings_skipped_duplicate = 0U;
    std::size_t mappings_skipped_ambiguous = 0U;
    std::size_t mappings_skipped_missing_texture = 0U;
    std::size_t mappings_skipped_unmatched = 0U;
    std::size_t mappings_skipped_if_missing_existing_texture = 0U;
    std::size_t mappings_skipped_rejected_path = 0U;
};

StaticTextureRemapApplicationResult
apply_static_texture_remap(const std::string& asset_path,
                           std::span<mesh_asset_loader::MeshAssetPrimitive> primitives);

} // namespace isla::client
