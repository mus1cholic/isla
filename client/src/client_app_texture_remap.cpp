#include "client_app_texture_remap.hpp"

#include "absl/strings/str_cat.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "isla/engine/render/pmx_texture_remap_sidecar.hpp"

namespace isla::client {
namespace {

std::string to_lower_ascii_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string mapping_target_key(const pmx_texture_remap_sidecar::Mapping& mapping) {
    if (mapping.target.material_name.has_value()) {
        return "material:" + to_lower_ascii_copy(*mapping.target.material_name);
    }
    if (mapping.target.mesh_index.has_value() && mapping.target.primitive_index.has_value()) {
        return "index:" + std::to_string(*mapping.target.mesh_index) + ":" +
               std::to_string(*mapping.target.primitive_index);
    }
    return "unknown_target";
}

std::string build_texture_remap_summary(const StaticTextureRemapApplicationResult& result) {
    return absl::StrCat("texture remap apply summary mappings_total=", result.mappings_total,
                        " mappings_applied=", result.mappings_applied,
                        " skipped_duplicate=", result.mappings_skipped_duplicate,
                        " skipped_ambiguous=", result.mappings_skipped_ambiguous,
                        " skipped_missing_texture=", result.mappings_skipped_missing_texture,
                        " skipped_unmatched=", result.mappings_skipped_unmatched,
                        " skipped_if_missing_existing_texture=",
                        result.mappings_skipped_if_missing_existing_texture,
                        " skipped_rejected_path=", result.mappings_skipped_rejected_path);
}

bool load_texture_remap_sidecar_for_asset(
    const std::string& asset_path, StaticTextureRemapApplicationResult& result,
    pmx_texture_remap_sidecar::SidecarLoadResult& sidecar_loaded_out) {
    std::filesystem::path sidecar_path(asset_path);
    sidecar_path.replace_extension(".texturemap.json");
    std::error_code exists_error;
    const bool sidecar_exists = std::filesystem::exists(sidecar_path, exists_error);
    if (exists_error) {
        result.warnings.push_back("failed checking texture remap sidecar path '" +
                                  sidecar_path.string() + "': " + exists_error.message());
        return false;
    }
    if (!sidecar_exists) {
        return false;
    }

    sidecar_loaded_out =
        pmx_texture_remap_sidecar::load_from_file(sidecar_path.string(), asset_path);
    result.mappings_total = sidecar_loaded_out.sidecar.mappings.size();
    for (const std::string& warning : sidecar_loaded_out.warnings) {
        result.warnings.push_back("texture remap sidecar warning: " + warning);
    }
    if (!sidecar_loaded_out.ok) {
        result.sidecar_load_failed = true;
        result.warnings.push_back("failed to load texture remap sidecar '" + sidecar_path.string() +
                                  "': " + sidecar_loaded_out.error_message);
        return false;
    }

    result.infos.push_back(
        "loaded texture remap sidecar '" + sidecar_path.string() +
        "' mappings=" + std::to_string(sidecar_loaded_out.sidecar.mappings.size()) + " mode=" +
        (sidecar_loaded_out.sidecar.override_mode == pmx_texture_remap_sidecar::OverrideMode::Always
             ? "always"
             : "if_missing"));
    return true;
}

std::vector<std::size_t>
find_mapping_matches(const pmx_texture_remap_sidecar::Mapping& mapping,
                     std::span<mesh_asset_loader::MeshAssetPrimitive> primitives) {
    std::vector<std::size_t> matches;
    matches.reserve(4U);
    if (mapping.target.material_name.has_value()) {
        const std::string target_name_lower = to_lower_ascii_copy(*mapping.target.material_name);
        for (std::size_t i = 0U; i < primitives.size(); ++i) {
            const std::string material_name_lower =
                to_lower_ascii_copy(primitives[i].source_material_name);
            if (!material_name_lower.empty() && material_name_lower == target_name_lower) {
                matches.push_back(i);
            }
        }
    } else if (mapping.target.mesh_index.has_value() &&
               mapping.target.primitive_index.has_value()) {
        for (std::size_t i = 0U; i < primitives.size(); ++i) {
            if (!primitives[i].has_source_identity) {
                continue;
            }
            if (primitives[i].source_mesh_index == *mapping.target.mesh_index &&
                primitives[i].source_primitive_index == *mapping.target.primitive_index) {
                matches.push_back(i);
            }
        }
    }
    return matches;
}

void apply_single_texture_mapping(const pmx_texture_remap_sidecar::Mapping& mapping,
                                  const pmx_texture_remap_sidecar::OverrideMode override_mode,
                                  std::span<mesh_asset_loader::MeshAssetPrimitive> primitives,
                                  std::unordered_set<std::string>& seen_mapping_keys,
                                  StaticTextureRemapApplicationResult& result) {
    const std::string mapping_key = mapping_target_key(mapping);
    if (!mapping_key.empty() && seen_mapping_keys.contains(mapping_key)) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' skipped due to duplicate key collision key='" + mapping_key +
                                  "'");
        ++result.mappings_skipped_duplicate;
        return;
    }
    if (!mapping_key.empty()) {
        seen_mapping_keys.insert(mapping_key);
    }

    const std::vector<std::size_t> matches = find_mapping_matches(mapping, primitives);
    if (matches.empty()) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' target did not match any primitive key='" + mapping_key + "'");
        ++result.mappings_skipped_unmatched;
        return;
    }
    if (matches.size() > 1U) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' is ambiguous and matched multiple primitives key='" +
                                  mapping_key + "'");
        ++result.mappings_skipped_ambiguous;
        return;
    }

    if (mapping.albedo_texture_path.empty()) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' has rejected or unsupported albedo texture path key='" +
                                  mapping_key + "'");
        ++result.mappings_skipped_rejected_path;
        return;
    }

    // Invariant: mapping.albedo_texture_path is pre-sanitized and resolved asset-relative by
    // pmx_texture_remap_sidecar::load_from_file via
    // mesh_asset_loader::resolve_asset_relative_texture_path.
    const std::filesystem::path texture_path(mapping.albedo_texture_path);
    std::error_code texture_exists_error;
    const bool texture_exists = std::filesystem::exists(texture_path, texture_exists_error);
    if (texture_exists_error) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' failed checking texture file '" + mapping.albedo_texture_path +
                                  "' key='" + mapping_key + "' error='" +
                                  texture_exists_error.message() + "'");
        ++result.mappings_skipped_missing_texture;
        return;
    }
    if (!texture_exists) {
        result.warnings.push_back("texture remap mapping id='" + mapping.id +
                                  "' points to missing texture file '" +
                                  mapping.albedo_texture_path + "' key='" + mapping_key + "'");
        ++result.mappings_skipped_missing_texture;
        return;
    }

    const std::size_t primitive_index = matches.front();
    mesh_asset_loader::MeshAssetPrimitive& primitive = primitives[primitive_index];
    if (override_mode == pmx_texture_remap_sidecar::OverrideMode::IfMissing &&
        !primitive.material.albedo_texture_path.empty()) {
        ++result.mappings_skipped_if_missing_existing_texture;
        result.infos.push_back("texture remap mapping id='" + mapping.id +
                               "' skipped by policy override_mode=if_missing because target "
                               "already has glTF texture key='" +
                               mapping_key + "'");
        return;
    }
    primitive.material.albedo_texture_path = mapping.albedo_texture_path;
    if (mapping.alpha_cutoff.has_value()) {
        primitive.material.alpha_cutoff = *mapping.alpha_cutoff;
    }
    result.applied_from_texturemap[primitive_index] = true;
    ++result.mappings_applied;
}

} // namespace

StaticTextureRemapApplicationResult
apply_static_texture_remap(const std::string& asset_path,
                           std::span<mesh_asset_loader::MeshAssetPrimitive> primitives) {
    StaticTextureRemapApplicationResult result;
    result.applied_from_texturemap.assign(primitives.size(), false);
    if (primitives.empty()) {
        return result;
    }

    pmx_texture_remap_sidecar::SidecarLoadResult sidecar_loaded;
    if (!load_texture_remap_sidecar_for_asset(asset_path, result, sidecar_loaded)) {
        return result;
    }

    std::unordered_set<std::string> seen_mapping_keys;
    seen_mapping_keys.reserve(sidecar_loaded.sidecar.mappings.size());
    for (const pmx_texture_remap_sidecar::Mapping& mapping : sidecar_loaded.sidecar.mappings) {
        apply_single_texture_mapping(mapping, sidecar_loaded.sidecar.override_mode, primitives,
                                     seen_mapping_keys, result);
    }

    result.infos.push_back(build_texture_remap_summary(result));
    return result;
}

} // namespace isla::client
