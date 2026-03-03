#include "engine/src/render/include/mesh_asset_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace isla::client::mesh_asset_loader {

namespace {

struct ObjFaceVertex {
    std::size_t position_index = 0U;
    std::optional<std::size_t> uv_index;
};

std::string trim_copy(std::string_view value) {
    std::size_t start = 0U;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::optional<std::size_t> resolve_obj_index(int index, std::size_t element_count) {
    if (index > 0) {
        const auto resolved = static_cast<std::size_t>(index - 1);
        if (resolved < element_count) {
            return resolved;
        }
        return std::nullopt;
    }
    if (index < 0) {
        if (index == std::numeric_limits<int>::min()) {
            return std::nullopt;
        }
        const auto magnitude = static_cast<std::size_t>(-index);
        if (magnitude <= element_count) {
            return element_count - magnitude;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ObjFaceVertex>
parse_obj_face_vertex(std::string_view token, std::size_t position_count, std::size_t uv_count) {
    const std::string token_string(token);
    const std::size_t first_slash = token_string.find('/');
    if (first_slash == std::string::npos) {
        std::istringstream ss(token_string);
        int position_index = 0;
        if (!(ss >> position_index)) {
            return std::nullopt;
        }
        const std::optional<std::size_t> resolved_position =
            resolve_obj_index(position_index, position_count);
        if (!resolved_position.has_value()) {
            return std::nullopt;
        }
        return ObjFaceVertex{ .position_index = *resolved_position, .uv_index = std::nullopt };
    }

    const std::string position_part = token_string.substr(0U, first_slash);
    const std::size_t second_slash = token_string.find('/', first_slash + 1U);
    const std::string uv_part =
        second_slash == std::string::npos
            ? token_string.substr(first_slash + 1U)
            : token_string.substr(first_slash + 1U, second_slash - (first_slash + 1U));

    std::istringstream position_stream(position_part);
    int position_index = 0;
    if (!(position_stream >> position_index)) {
        return std::nullopt;
    }
    const std::optional<std::size_t> resolved_position =
        resolve_obj_index(position_index, position_count);
    if (!resolved_position.has_value()) {
        return std::nullopt;
    }

    std::optional<std::size_t> resolved_uv;
    if (!uv_part.empty()) {
        std::istringstream uv_stream(uv_part);
        int uv_index = 0;
        if (!(uv_stream >> uv_index)) {
            return std::nullopt;
        }
        resolved_uv = resolve_obj_index(uv_index, uv_count);
        if (!resolved_uv.has_value()) {
            return std::nullopt;
        }
    }

    return ObjFaceVertex{
        .position_index = *resolved_position,
        .uv_index = resolved_uv,
    };
}

MeshAssetLoadResult load_obj(std::string_view asset_path) {
    std::ifstream stream{ std::string(asset_path) };
    if (!stream.is_open()) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "failed to open OBJ file",
        };
    }

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Triangle> triangles;
    std::string line;

    while (std::getline(stream, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        std::istringstream line_stream(trimmed);
        std::string directive;
        line_stream >> directive;
        if (directive == "v") {
            Vec3 position{};
            if (!(line_stream >> position.x >> position.y >> position.z)) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "invalid OBJ vertex line",
                };
            }
            positions.push_back(position);
            continue;
        }

        if (directive == "vt") {
            Vec2 uv{};
            if (!(line_stream >> uv.x >> uv.y)) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "invalid OBJ texcoord line",
                };
            }
            uvs.push_back(uv);
            continue;
        }

        if (directive != "f") {
            continue;
        }

        std::vector<ObjFaceVertex> face_vertices;
        std::string token;
        while (line_stream >> token) {
            const std::optional<ObjFaceVertex> parsed =
                parse_obj_face_vertex(token, positions.size(), uvs.size());
            if (!parsed.has_value()) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "invalid OBJ face index",
                };
            }
            face_vertices.push_back(*parsed);
        }

        if (face_vertices.size() < 3U) {
            return MeshAssetLoadResult{
                .ok = false,
                .triangles = {},
                .error_message = "OBJ face requires at least 3 vertices",
            };
        }

        const auto read_uv = [&](const ObjFaceVertex& vertex) -> Vec2 {
            if (!vertex.uv_index.has_value()) {
                return Vec2{};
            }
            return uvs.at(*vertex.uv_index);
        };

        for (std::size_t i = 1U; i + 1U < face_vertices.size(); ++i) {
            const ObjFaceVertex& v0 = face_vertices.at(0U);
            const ObjFaceVertex& v1 = face_vertices.at(i);
            const ObjFaceVertex& v2 = face_vertices.at(i + 1U);
            triangles.push_back(Triangle{
                .a = positions.at(v0.position_index),
                .b = positions.at(v1.position_index),
                .c = positions.at(v2.position_index),
                .uv_a = read_uv(v0),
                .uv_b = read_uv(v1),
                .uv_c = read_uv(v2),
            });
        }
    }

    if (triangles.empty()) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "OBJ file contains no triangles",
        };
    }

    return MeshAssetLoadResult{
        .ok = true,
        .triangles = std::move(triangles),
        .error_message = {},
    };
}

class CgltfDataDeleter {
  public:
    void operator()(cgltf_data* data) const {
        if (data != nullptr) {
            cgltf_free(data);
        }
    }
};

using CgltfDataPtr = std::unique_ptr<cgltf_data, CgltfDataDeleter>;

std::optional<Vec3> read_vec3(const cgltf_accessor* accessor, cgltf_size index) {
    std::array<float, 3U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    return Vec3{ .x = values[0], .y = values[1], .z = values[2] };
}

std::optional<Vec2> read_vec2(const cgltf_accessor* accessor, cgltf_size index) {
    std::array<float, 2U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    return Vec2{ .x = values[0], .y = values[1] };
}

MeshAssetLoadResult load_gltf(std::string_view asset_path) {
    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    const cgltf_result parse_result =
        cgltf_parse_file(&options, std::string(asset_path).c_str(), &parsed);
    if (parse_result != cgltf_result_success || parsed == nullptr) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "failed to parse glTF/GLB file",
        };
    }

    CgltfDataPtr data(parsed);
    if (cgltf_load_buffers(&options, data.get(), std::string(asset_path).c_str()) !=
        cgltf_result_success) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "failed to load glTF buffers",
        };
    }

    if (data->meshes_count == 0U) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "glTF has no mesh primitives",
        };
    }
    std::vector<Triangle> triangles;
    bool found_triangle_primitive = false;
    for (cgltf_size mesh_i = 0U; mesh_i < data->meshes_count; ++mesh_i) {
        const cgltf_mesh& mesh = data->meshes[mesh_i];
        for (cgltf_size prim_i = 0U; prim_i < mesh.primitives_count; ++prim_i) {
            const cgltf_primitive& primitive = mesh.primitives[prim_i];
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;
            }
            found_triangle_primitive = true;

            const cgltf_accessor* position_accessor = nullptr;
            const cgltf_accessor* texcoord_accessor = nullptr;
            for (cgltf_size attr_i = 0U; attr_i < primitive.attributes_count; ++attr_i) {
                const cgltf_attribute& attribute = primitive.attributes[attr_i];
                if (attribute.type == cgltf_attribute_type_position) {
                    position_accessor = attribute.data;
                } else if (attribute.type == cgltf_attribute_type_texcoord &&
                           attribute.index == 0U) {
                    texcoord_accessor = attribute.data;
                }
            }

            if (position_accessor == nullptr) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "glTF primitive has no POSITION accessor",
                };
            }
            if (position_accessor->type != cgltf_type_vec3 ||
                position_accessor->component_type != cgltf_component_type_r_32f) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "glTF POSITION accessor must be float VEC3",
                };
            }

            if (texcoord_accessor != nullptr &&
                (texcoord_accessor->type != cgltf_type_vec2 ||
                 texcoord_accessor->component_type != cgltf_component_type_r_32f)) {
                return MeshAssetLoadResult{
                    .ok = false,
                    .triangles = {},
                    .error_message = "glTF TEXCOORD_0 accessor must be float VEC2",
                };
            }

            if (primitive.indices != nullptr) {
                const cgltf_accessor* indices_accessor = primitive.indices;
                if (indices_accessor->count < 3U || (indices_accessor->count % 3U) != 0U) {
                    return MeshAssetLoadResult{
                        .ok = false,
                        .triangles = {},
                        .error_message =
                            "indexed glTF primitive index count must be a multiple of 3",
                    };
                }

                for (cgltf_size i = 0U; i < indices_accessor->count; i += 3U) {
                    const cgltf_size ia = cgltf_accessor_read_index(indices_accessor, i);
                    const cgltf_size ib = cgltf_accessor_read_index(indices_accessor, i + 1U);
                    const cgltf_size ic = cgltf_accessor_read_index(indices_accessor, i + 2U);
                    if (ia >= position_accessor->count || ib >= position_accessor->count ||
                        ic >= position_accessor->count) {
                        return MeshAssetLoadResult{
                            .ok = false,
                            .triangles = {},
                            .error_message = "glTF index points past POSITION accessor bounds",
                        };
                    }

                    const std::optional<Vec3> a = read_vec3(position_accessor, ia);
                    const std::optional<Vec3> b = read_vec3(position_accessor, ib);
                    const std::optional<Vec3> c = read_vec3(position_accessor, ic);
                    if (!a.has_value() || !b.has_value() || !c.has_value()) {
                        return MeshAssetLoadResult{
                            .ok = false,
                            .triangles = {},
                            .error_message = "failed reading glTF POSITION values",
                        };
                    }

                    Vec2 uv_a{};
                    Vec2 uv_b{};
                    Vec2 uv_c{};
                    if (texcoord_accessor != nullptr) {
                        const std::optional<Vec2> ta = ia < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, ia)
                                                           : std::optional<Vec2>{};
                        const std::optional<Vec2> tb = ib < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, ib)
                                                           : std::optional<Vec2>{};
                        const std::optional<Vec2> tc = ic < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, ic)
                                                           : std::optional<Vec2>{};
                        if (!ta.has_value() || !tb.has_value() || !tc.has_value()) {
                            return MeshAssetLoadResult{
                                .ok = false,
                                .triangles = {},
                                .error_message = "failed reading glTF TEXCOORD_0 values",
                            };
                        }
                        uv_a = *ta;
                        uv_b = *tb;
                        uv_c = *tc;
                    }

                    triangles.push_back(Triangle{
                        .a = *a,
                        .b = *b,
                        .c = *c,
                        .uv_a = uv_a,
                        .uv_b = uv_b,
                        .uv_c = uv_c,
                    });
                }
            } else {
                if (position_accessor->count < 3U || (position_accessor->count % 3U) != 0U) {
                    return MeshAssetLoadResult{
                        .ok = false,
                        .triangles = {},
                        .error_message = "non-indexed glTF POSITION count must be a multiple of 3",
                    };
                }

                for (cgltf_size i = 0U; i < position_accessor->count; i += 3U) {
                    const std::optional<Vec3> a = read_vec3(position_accessor, i);
                    const std::optional<Vec3> b = read_vec3(position_accessor, i + 1U);
                    const std::optional<Vec3> c = read_vec3(position_accessor, i + 2U);
                    if (!a.has_value() || !b.has_value() || !c.has_value()) {
                        return MeshAssetLoadResult{
                            .ok = false,
                            .triangles = {},
                            .error_message = "failed reading glTF POSITION values",
                        };
                    }

                    Vec2 uv_a{};
                    Vec2 uv_b{};
                    Vec2 uv_c{};
                    if (texcoord_accessor != nullptr) {
                        const std::optional<Vec2> ta = i < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, i)
                                                           : std::optional<Vec2>{};
                        const std::optional<Vec2> tb = (i + 1U) < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, i + 1U)
                                                           : std::optional<Vec2>{};
                        const std::optional<Vec2> tc = (i + 2U) < texcoord_accessor->count
                                                           ? read_vec2(texcoord_accessor, i + 2U)
                                                           : std::optional<Vec2>{};
                        if (!ta.has_value() || !tb.has_value() || !tc.has_value()) {
                            return MeshAssetLoadResult{
                                .ok = false,
                                .triangles = {},
                                .error_message = "failed reading glTF TEXCOORD_0 values",
                            };
                        }
                        uv_a = *ta;
                        uv_b = *tb;
                        uv_c = *tc;
                    }

                    triangles.push_back(Triangle{
                        .a = *a,
                        .b = *b,
                        .c = *c,
                        .uv_a = uv_a,
                        .uv_b = uv_b,
                        .uv_c = uv_c,
                    });
                }
            }
        }
    }

    if (!found_triangle_primitive) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "glTF has no triangle primitive",
        };
    }

    if (triangles.empty()) {
        return MeshAssetLoadResult{
            .ok = false,
            .triangles = {},
            .error_message = "glTF contains no triangles",
        };
    }

    return MeshAssetLoadResult{
        .ok = true,
        .triangles = std::move(triangles),
        .error_message = {},
    };
}

} // namespace

MeshAssetLoadResult load_from_file(std::string_view asset_path) {
    const std::filesystem::path path(asset_path);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == ".obj") {
        return load_obj(asset_path);
    }
    if (extension == ".gltf" || extension == ".glb") {
        return load_gltf(asset_path);
    }

    return MeshAssetLoadResult{
        .ok = false,
        .triangles = {},
        .error_message = "unsupported mesh asset extension",
    };
}

} // namespace isla::client::mesh_asset_loader

