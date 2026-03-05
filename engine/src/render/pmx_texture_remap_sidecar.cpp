#include "engine/src/render/include/pmx_texture_remap_sidecar.hpp"

#include "engine/src/render/include/mesh_asset_loader.hpp"

#include "absl/log/log.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace isla::client::pmx_texture_remap_sidecar {
namespace {

using json = nlohmann::json;

std::string make_sidecar_error(std::string_view sidecar_path, std::string_view detail) {
    return "texture remap sidecar '" + std::string(sidecar_path) + "': " + std::string(detail);
}

const json* object_find(const json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return nullptr;
    }
    return &(*it);
}

std::optional<std::string> read_string(const json& object, std::string_view key) {
    const json* value = object_find(object, key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    const std::string parsed = value->get<std::string>();
    if (parsed.empty() || parsed.size() > kMaxStringLengthBytes) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> read_number(const json& object, std::string_view key) {
    const json* value = object_find(object, key);
    if (value == nullptr || !value->is_number()) {
        return std::nullopt;
    }
    const double parsed = value->get<double>();
    if (!std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::size_t> parse_non_negative_integer(const json& value) {
    if (value.is_number_unsigned()) {
        const std::uint64_t number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(number);
    }
    if (value.is_number_integer()) {
        const std::int64_t number = value.get<std::int64_t>();
        if (number < 0) {
            return std::nullopt;
        }
        const std::uint64_t as_u64 = static_cast<std::uint64_t>(number);
        if (as_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(as_u64);
    }
    return std::nullopt;
}

} // namespace

SidecarLoadResult load_from_file(std::string_view sidecar_path, std::string_view asset_path) {
    SidecarLoadResult result{};
    const std::filesystem::path file_path(sidecar_path);
    std::error_code file_size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(file_path, file_size_error);
    if (!file_size_error && file_size > static_cast<std::uintmax_t>(kMaxSidecarFileSizeBytes)) {
        result.error_message =
            make_sidecar_error(sidecar_path, "file size exceeds maximum allowed bytes");
        return result;
    }

    std::ifstream stream(std::string(sidecar_path), std::ios::binary);
    if (!stream.is_open()) {
        result.error_message = make_sidecar_error(sidecar_path, "failed to open file");
        return result;
    }

    std::string json_text;
    constexpr std::size_t kReadChunkBytes = 4096U;
    std::array<char, kReadChunkBytes> chunk{};
    std::size_t bytes_read_total = 0U;
    while (stream.good()) {
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize bytes_read = stream.gcount();
        if (bytes_read <= 0) {
            break;
        }
        bytes_read_total += static_cast<std::size_t>(bytes_read);
        if (bytes_read_total > kMaxSidecarFileSizeBytes) {
            result.error_message =
                make_sidecar_error(sidecar_path, "file size exceeds maximum allowed bytes");
            return result;
        }
        json_text.append(chunk.data(), static_cast<std::size_t>(bytes_read));
    }
    if (stream.bad()) {
        result.error_message = make_sidecar_error(sidecar_path, "failed while reading file");
        return result;
    }

    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::parse_error& e) {
        result.error_message = make_sidecar_error(sidecar_path, "failed to parse JSON (" +
                                                                    std::string(e.what()) + ")");
        return result;
    }
    if (!root.is_object()) {
        result.error_message =
            make_sidecar_error(sidecar_path, "top-level JSON value must be an object");
        return result;
    }

    const std::optional<std::string> schema_version = read_string(root, "schema_version");
    if (!schema_version.has_value()) {
        result.error_message = make_sidecar_error(sidecar_path, "missing schema_version");
        return result;
    }
    if (*schema_version != kExpectedSchemaVersion) {
        result.error_message = make_sidecar_error(
            sidecar_path, "schema_version is unsupported: got '" + *schema_version +
                              "', expected '" + std::string(kExpectedSchemaVersion) + "'");
        return result;
    }

    const json* policy = object_find(root, "policy");
    if (policy == nullptr || !policy->is_object()) {
        result.error_message =
            make_sidecar_error(sidecar_path, "policy section missing or invalid");
        return result;
    }

    const std::optional<std::string> override_mode = read_string(*policy, "override_mode");
    const std::optional<std::string> path_scope = read_string(*policy, "path_scope");
    if (!override_mode.has_value() || !path_scope.has_value()) {
        result.error_message =
            make_sidecar_error(sidecar_path, "policy must contain override_mode and path_scope");
        return result;
    }
    if (*path_scope != "asset_relative_only") {
        result.error_message =
            make_sidecar_error(sidecar_path, "policy.path_scope must be 'asset_relative_only'");
        return result;
    }

    SidecarData parsed{};
    if (*override_mode == "if_missing") {
        parsed.override_mode = OverrideMode::IfMissing;
    } else if (*override_mode == "always") {
        parsed.override_mode = OverrideMode::Always;
    } else {
        result.error_message = make_sidecar_error(
            sidecar_path, "policy.override_mode must be 'if_missing' or 'always'");
        return result;
    }

    const json* mappings = object_find(root, "mappings");
    if (mappings == nullptr || !mappings->is_array()) {
        result.error_message =
            make_sidecar_error(sidecar_path, "mappings section missing or invalid");
        return result;
    }
    if (mappings->size() > kMaxMappings) {
        result.error_message = make_sidecar_error(sidecar_path, "mappings exceeds maximum count");
        return result;
    }
    parsed.mappings.reserve(mappings->size());

    for (std::size_t i = 0U; i < mappings->size(); ++i) {
        const json& entry = (*mappings)[i];
        if (!entry.is_object()) {
            result.error_message =
                make_sidecar_error(sidecar_path, "mapping entry is not an object");
            return result;
        }

        const std::optional<std::string> id = read_string(entry, "id");
        if (!id.has_value()) {
            result.error_message =
                make_sidecar_error(sidecar_path, "mapping entry missing valid id");
            return result;
        }
        const std::optional<std::string> albedo_texture = read_string(entry, "albedo_texture");
        if (!albedo_texture.has_value()) {
            result.error_message =
                make_sidecar_error(sidecar_path, "mapping entry missing valid albedo_texture");
            return result;
        }

        const json* target = object_find(entry, "target");
        if (target == nullptr || !target->is_object()) {
            result.error_message =
                make_sidecar_error(sidecar_path, "mapping entry target is missing or invalid");
            return result;
        }

        Mapping mapping{};
        mapping.id = *id;
        if (const std::optional<std::string> material_name = read_string(*target, "material_name");
            material_name.has_value()) {
            mapping.target.material_name = *material_name;
        }
        if (const json* mesh_value = object_find(*target, "mesh_index"); mesh_value != nullptr) {
            const std::optional<std::size_t> mesh_index = parse_non_negative_integer(*mesh_value);
            if (!mesh_index.has_value()) {
                result.error_message =
                    make_sidecar_error(sidecar_path, "mapping target.mesh_index is invalid");
                return result;
            }
            mapping.target.mesh_index = *mesh_index;
        }
        if (const json* primitive_value = object_find(*target, "primitive_index");
            primitive_value != nullptr) {
            const std::optional<std::size_t> primitive_index =
                parse_non_negative_integer(*primitive_value);
            if (!primitive_index.has_value()) {
                result.error_message =
                    make_sidecar_error(sidecar_path, "mapping target.primitive_index is invalid");
                return result;
            }
            mapping.target.primitive_index = *primitive_index;
        }

        const bool has_material_name = mapping.target.material_name.has_value();
        const bool has_mesh_primitive =
            mapping.target.mesh_index.has_value() && mapping.target.primitive_index.has_value();
        if (has_material_name == has_mesh_primitive) {
            result.error_message = make_sidecar_error(
                sidecar_path, "mapping target must specify exactly one key mode");
            return result;
        }

        mapping.albedo_texture_path =
            mesh_asset_loader::resolve_asset_relative_texture_path(asset_path, *albedo_texture);
        if (mapping.albedo_texture_path.empty()) {
            result.warnings.push_back("mapping id='" + mapping.id +
                                      "' has rejected or unsupported albedo_texture path");
        }

        if (const std::optional<double> alpha_cutoff = read_number(entry, "alpha_cutoff");
            alpha_cutoff.has_value()) {
            if (*alpha_cutoff < 0.0 || *alpha_cutoff > 1.0) {
                result.error_message =
                    make_sidecar_error(sidecar_path, "mapping alpha_cutoff must be in [0,1]");
                return result;
            }
            mapping.alpha_cutoff = static_cast<float>(*alpha_cutoff);
        }

        parsed.mappings.push_back(std::move(mapping));
    }

    result.ok = true;
    result.sidecar = std::move(parsed);
    VLOG(1) << "PmxTextureRemapSidecar: loaded '" << sidecar_path
            << "' mappings=" << result.sidecar.mappings.size() << " override_mode="
            << (result.sidecar.override_mode == OverrideMode::Always ? "always" : "if_missing")
            << " warnings=" << result.warnings.size();
    return result;
}

} // namespace isla::client::pmx_texture_remap_sidecar
