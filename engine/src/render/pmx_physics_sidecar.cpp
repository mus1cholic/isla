#include "engine/src/render/include/pmx_physics_sidecar.hpp"

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace isla::client::pmx_physics_sidecar {

namespace {

using json = nlohmann::json;

std::string make_sidecar_error(std::string_view sidecar_path, std::string_view detail) {
    return "physics sidecar '" + std::string(sidecar_path) + "': " + std::string(detail);
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

std::optional<std::string> read_required_string(const json& object, std::string_view key) {
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

std::optional<double> read_required_number(const json& object, std::string_view key) {
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

std::optional<bool> read_required_bool(const json& object, std::string_view key) {
    const json* value = object_find(object, key);
    if (value == nullptr || !value->is_boolean()) {
        return std::nullopt;
    }
    return value->get<bool>();
}

std::optional<std::uint32_t> read_required_u32(const json& object, std::string_view key) {
    const std::optional<double> number = read_required_number(object, key);
    if (!number.has_value()) {
        return std::nullopt;
    }
    if (*number < 0.0 || *number > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    const double rounded = std::round(*number);
    if (std::abs(rounded - *number) > 1.0e-6) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(rounded);
}

std::optional<Vec3> read_required_vec3(const json& object, std::string_view key) {
    const json* value = object_find(object, key);
    if (value == nullptr || !value->is_array() || value->size() != 3U) {
        return std::nullopt;
    }
    if (!(*value)[0].is_number() || !(*value)[1].is_number() || !(*value)[2].is_number()) {
        return std::nullopt;
    }
    const double x = (*value)[0].get<double>();
    const double y = (*value)[1].get<double>();
    const double z = (*value)[2].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return std::nullopt;
    }
    return Vec3{
        .x = static_cast<float>(x),
        .y = static_cast<float>(y),
        .z = static_cast<float>(z),
    };
}

bool parse_collision_layers(const json& root, SidecarData& out_data,
                            std::vector<std::string>& warnings, std::string* failure_reason) {
    const json* layers_value = object_find(root, "collision_layers");
    if (layers_value == nullptr || !layers_value->is_array()) {
        warnings.emplace_back("physics sidecar missing valid collision_layers array");
        if (failure_reason != nullptr) {
            *failure_reason = "missing/invalid collision_layers array";
        }
        return false;
    }
    if (layers_value->size() > kMaxCollisionLayers) {
        const std::string reason = absl::StrCat("collision_layers count ", layers_value->size(),
                                                " exceeds maximum allowed ", kMaxCollisionLayers);
        warnings.emplace_back(absl::StrCat("physics sidecar ", reason));
        if (failure_reason != nullptr) {
            *failure_reason = reason;
        }
        return false;
    }

    for (const json& layer : *layers_value) {
        if (!layer.is_object()) {
            warnings.emplace_back("physics sidecar collision_layers entry is not an object");
            continue;
        }
        const std::optional<std::uint32_t> index = read_required_u32(layer, "index");
        const std::optional<std::string> name = read_required_string(layer, "name");
        if (!index.has_value() || !name.has_value()) {
            warnings.emplace_back("physics sidecar collision_layers entry missing index/name");
            continue;
        }
        if (*index > kMaxCollisionLayerIndex) {
            warnings.emplace_back(
                absl::StrCat("physics sidecar collision_layers index must be in [0,",
                             kMaxCollisionLayerIndex, "]"));
            continue;
        }
        out_data.collision_layers.push_back(CollisionLayer{
            .index = *index,
            .name = *name,
        });
    }
    return true;
}

bool parse_constraints(const json& root, SidecarData& out_data, std::vector<std::string>& warnings,
                       std::string* failure_reason) {
    const json* constraints_value = object_find(root, "constraints");
    if (constraints_value == nullptr || !constraints_value->is_array()) {
        warnings.emplace_back("physics sidecar missing valid constraints array");
        if (failure_reason != nullptr) {
            *failure_reason = "missing/invalid constraints array";
        }
        return false;
    }
    if (constraints_value->size() > kMaxConstraints) {
        const std::string reason = absl::StrCat("constraints count ", constraints_value->size(),
                                                " exceeds maximum allowed ", kMaxConstraints);
        warnings.emplace_back(absl::StrCat("physics sidecar ", reason));
        if (failure_reason != nullptr) {
            *failure_reason = reason;
        }
        return false;
    }

    std::size_t skipped_unsupported = 0U;
    for (const json& entry : *constraints_value) {
        if (!entry.is_object()) {
            warnings.emplace_back("physics sidecar constraint entry is not an object");
            continue;
        }
        const std::optional<std::string> id = read_required_string(entry, "id");
        const std::optional<std::string> bone_a_name = read_required_string(entry, "bone_a_name");
        const std::optional<std::string> bone_b_name = read_required_string(entry, "bone_b_name");
        const std::optional<std::string> type = read_required_string(entry, "type");
        if (!id.has_value() || !bone_a_name.has_value() || !bone_b_name.has_value() ||
            !type.has_value()) {
            warnings.emplace_back("physics sidecar constraint missing required fields");
            continue;
        }

        ConstraintType parsed_type = ConstraintType::Fixed;
        if (*type == "fixed") {
            parsed_type = ConstraintType::Fixed;
        } else if (*type == "hinge") {
            parsed_type = ConstraintType::Hinge;
        } else if (*type == "cone_twist") {
            parsed_type = ConstraintType::ConeTwist;
        } else {
            warnings.emplace_back(absl::StrCat("physics sidecar constraint type '", *type,
                                               "' is unsupported and will be ignored"));
            ++skipped_unsupported;
            continue;
        }

        out_data.constraints.push_back(Constraint{
            .id = *id,
            .bone_a_name = *bone_a_name,
            .bone_b_name = *bone_b_name,
            .type = parsed_type,
        });
    }
    if (skipped_unsupported > 0U) {
        VLOG(1) << "PmxPhysicsSidecar: constraints skipped due to unsupported type count="
                << skipped_unsupported;
    }
    return true;
}

bool parse_colliders(const json& root, std::span<const std::string> joint_names,
                     SidecarData& out_data, std::vector<std::string>& warnings,
                     std::string* failure_reason) {
    const json* colliders_value = object_find(root, "colliders");
    if (colliders_value == nullptr || !colliders_value->is_array()) {
        warnings.emplace_back("physics sidecar missing valid colliders array");
        if (failure_reason != nullptr) {
            *failure_reason = "missing/invalid colliders array";
        }
        return false;
    }
    if (colliders_value->size() > kMaxColliders) {
        const std::string reason = absl::StrCat("colliders count ", colliders_value->size(),
                                                " exceeds maximum allowed ", kMaxColliders);
        warnings.emplace_back(absl::StrCat("physics sidecar ", reason));
        if (failure_reason != nullptr) {
            *failure_reason = reason;
        }
        return false;
    }

    std::unordered_set<std::string> known_joint_names;
    known_joint_names.reserve(joint_names.size());
    for (const std::string& joint_name : joint_names) {
        if (!joint_name.empty()) {
            known_joint_names.insert(joint_name);
        }
    }

    for (const json& entry : *colliders_value) {
        if (!entry.is_object()) {
            warnings.emplace_back("physics sidecar collider entry is not an object");
            continue;
        }
        const std::optional<std::string> id = read_required_string(entry, "id");
        const std::optional<std::string> bone_name = read_required_string(entry, "bone_name");
        const std::optional<std::string> shape = read_required_string(entry, "shape");
        const std::optional<Vec3> offset = read_required_vec3(entry, "offset");
        const std::optional<Vec3> rotation = read_required_vec3(entry, "rotation_euler_deg");
        const std::optional<bool> is_trigger = read_required_bool(entry, "is_trigger");
        const std::optional<std::uint32_t> layer = read_required_u32(entry, "layer");
        const std::optional<std::uint32_t> mask = read_required_u32(entry, "mask");
        if (!id.has_value() || !bone_name.has_value() || !shape.has_value() ||
            !offset.has_value() || !rotation.has_value() || !is_trigger.has_value() ||
            !layer.has_value() || !mask.has_value()) {
            warnings.emplace_back("physics sidecar collider missing required fields");
            continue;
        }
        if (*layer > kMaxCollisionLayerIndex) {
            warnings.emplace_back(absl::StrCat("physics sidecar collider layer ", *layer,
                                               " exceeds maximum collision-layer index ",
                                               kMaxCollisionLayerIndex));
            continue;
        }
        if (!known_joint_names.empty() && !known_joint_names.contains(*bone_name)) {
            warnings.emplace_back(absl::StrCat(
                "physics sidecar collider references unknown bone_name '", *bone_name, "'"));
            continue;
        }

        Collider parsed{};
        parsed.id = *id;
        parsed.bone_name = *bone_name;
        parsed.offset = *offset;
        parsed.rotation_euler_deg = *rotation;
        parsed.is_trigger = *is_trigger;
        parsed.layer = *layer;
        parsed.mask = *mask;

        if (*shape == "sphere") {
            parsed.shape = ColliderShape::Sphere;
            const std::optional<double> radius = read_required_number(entry, "radius");
            if (!radius.has_value() || *radius <= 0.0 || !std::isfinite(*radius)) {
                warnings.emplace_back("physics sidecar sphere collider missing valid radius");
                continue;
            }
            parsed.radius = static_cast<float>(*radius);
        } else if (*shape == "capsule") {
            parsed.shape = ColliderShape::Capsule;
            const std::optional<double> radius = read_required_number(entry, "radius");
            const std::optional<double> height = read_required_number(entry, "height");
            if (!radius.has_value() || !height.has_value() || *radius <= 0.0 || *height <= 0.0 ||
                !std::isfinite(*radius) || !std::isfinite(*height)) {
                warnings.emplace_back(
                    "physics sidecar capsule collider missing valid radius/height");
                continue;
            }
            parsed.radius = static_cast<float>(*radius);
            parsed.height = static_cast<float>(*height);
        } else if (*shape == "box") {
            parsed.shape = ColliderShape::Box;
            const std::optional<Vec3> size = read_required_vec3(entry, "size");
            if (!size.has_value() || size->x <= 0.0F || size->y <= 0.0F || size->z <= 0.0F) {
                warnings.emplace_back("physics sidecar box collider missing valid size");
                continue;
            }
            parsed.size = *size;
        } else {
            warnings.emplace_back(
                absl::StrCat("physics sidecar collider shape '", *shape, "' is unsupported"));
            continue;
        }

        out_data.colliders.push_back(std::move(parsed));
    }
    return true;
}

} // namespace

SidecarLoadResult load_from_file(std::string_view sidecar_path,
                                 std::span<const std::string> joint_names) {
    SidecarLoadResult result{};
    const std::filesystem::path file_path(sidecar_path);
    std::error_code file_size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(file_path, file_size_error);
    if (!file_size_error && file_size > static_cast<std::uintmax_t>(kMaxSidecarFileSizeBytes)) {
        result.error_message = make_sidecar_error(
            sidecar_path, absl::StrCat("file size ", file_size, " bytes exceeds maximum allowed ",
                                       kMaxSidecarFileSizeBytes, " bytes"));
        return result;
    }

    std::ifstream stream(std::string(sidecar_path), std::ios::binary);
    if (!stream.is_open()) {
        result.error_message = make_sidecar_error(sidecar_path, "failed to open file");
        return result;
    }

    std::string json_text;
    constexpr std::size_t kReadChunkBytes = 4096U;
    char chunk[kReadChunkBytes];
    std::size_t bytes_read_total = 0U;
    while (stream.good()) {
        stream.read(chunk, static_cast<std::streamsize>(kReadChunkBytes));
        const std::streamsize bytes_read = stream.gcount();
        if (bytes_read <= 0) {
            break;
        }
        bytes_read_total += static_cast<std::size_t>(bytes_read);
        if (bytes_read_total > kMaxSidecarFileSizeBytes) {
            result.error_message =
                make_sidecar_error(sidecar_path, absl::StrCat("file size exceeds maximum allowed ",
                                                              kMaxSidecarFileSizeBytes, " bytes"));
            return result;
        }
        json_text.append(chunk, static_cast<std::size_t>(bytes_read));
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

    const std::optional<std::string> schema_version = read_required_string(root, "schema_version");
    if (!schema_version.has_value()) {
        result.error_message = make_sidecar_error(sidecar_path, "missing schema_version");
        return result;

        if (*schema_version != kExpectedSchemaVersion) {
            result.error_message = make_sidecar_error(
                sidecar_path, "schema_version is unsupported: got '" + *schema_version +
                                  "', expected '" + kExpectedSchemaVersion + "'");
            return result;
        }

        const json* converter = object_find(root, "converter");
        if (converter == nullptr || !converter->is_object()) {
            result.error_message =
                make_sidecar_error(sidecar_path, "converter section missing or invalid");
            return result;
        }

        SidecarData sidecar;
        std::string top_level_failure_reason;
        if (!parse_collision_layers(root, sidecar, result.warnings, &top_level_failure_reason)) {
            result.error_message = make_sidecar_error(
                sidecar_path, top_level_failure_reason.empty() ? "missing required top-level arrays"
                                                               : top_level_failure_reason);
            return result;
        }
        if (!parse_colliders(root, joint_names, sidecar, result.warnings,
                             &top_level_failure_reason)) {
            result.error_message = make_sidecar_error(
                sidecar_path, top_level_failure_reason.empty() ? "missing required top-level arrays"
                                                               : top_level_failure_reason);
            return result;
        }
        if (!parse_constraints(root, sidecar, result.warnings, &top_level_failure_reason)) {
            result.error_message = make_sidecar_error(
                sidecar_path, top_level_failure_reason.empty() ? "missing required top-level arrays"
                                                               : top_level_failure_reason);
            return result;
        }

        result.ok = true;
        result.sidecar = std::move(sidecar);
        VLOG(1) << "PmxPhysicsSidecar: loaded '" << sidecar_path
                << "' colliders=" << result.sidecar.colliders.size()
                << " constraints=" << result.sidecar.constraints.size()
                << " collision_layers=" << result.sidecar.collision_layers.size()
                << " warnings=" << result.warnings.size();
        return result;
    }
}

} // namespace isla::client::pmx_physics_sidecar
