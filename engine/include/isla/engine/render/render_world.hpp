#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/math/transform.hpp"
#include "isla/engine/render/render_types.hpp"

namespace isla::client {

inline constexpr Color3 kDefaultObjectColor{ .r = 1.0F, .g = 1.0F, .b = 1.0F };
inline constexpr Color3 kDefaultAmbientColor{ .r = 0.2F, .g = 0.2F, .b = 0.2F };
inline constexpr Color3 kDefaultDirectionalLightColor{ .r = 0.8F, .g = 0.8F, .b = 0.8F };
inline constexpr Vec3 kDefaultDirectionalLightDirection{ .x = -0.35F, .y = 0.85F, .z = -0.35F };

struct AmbientLight {
    Color3 color = kDefaultAmbientColor;
};

struct DirectionalLight {
    Vec3 direction = kDefaultDirectionalLightDirection;
    Color3 color = kDefaultDirectionalLightColor;
};

struct BoundingSphere {
    Vec3 center{};
    float radius = 0.0F;
};

struct SkinnedMeshVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
    std::array<std::uint16_t, 4U> joints{ 0U, 0U, 0U, 0U };
    std::array<float, 4U> weights{ 1.0F, 0.0F, 0.0F, 0.0F };
};

class MeshData {
  public:
    using TriangleList = std::vector<Triangle>;
    using SkinnedVertexList = std::vector<SkinnedMeshVertex>;
    using IndexList = std::vector<std::uint32_t>;

    [[nodiscard]] const TriangleList& triangles() const {
        return triangles_;
    }

    [[nodiscard]] const SkinnedVertexList& skinned_vertices() const {
        return skinned_vertices_;
    }

    [[nodiscard]] const IndexList& skinned_indices() const {
        return skinned_indices_;
    }

    [[nodiscard]] bool has_skinned_geometry() const {
        return !skinned_vertices_.empty() && !skinned_indices_.empty();
    }

    [[nodiscard]] const std::vector<Mat4>& skin_palette() const {
        return skin_palette_;
    }

    void set_skinned_geometry(SkinnedVertexList vertices, IndexList indices) {
        skinned_vertices_ = std::move(vertices);
        skinned_indices_ = std::move(indices);
        mark_geometry_dirty();
    }

    void clear_skinned_geometry() {
        if (skinned_vertices_.empty() && skinned_indices_.empty()) {
            return;
        }
        skinned_vertices_.clear();
        skinned_indices_.clear();
        mark_geometry_dirty();
    }

    void set_skin_palette(std::vector<Mat4> skin_palette) {
        skin_palette_ = std::move(skin_palette);
    }

    [[nodiscard]] const BoundingSphere& local_bounds() const {
        return local_bounds_;
    }

    [[nodiscard]] std::uint64_t geometry_revision() const {
        return geometry_revision_;
    }

    template <typename Fn> void edit_triangles(Fn&& fn) {
        static_assert(std::is_void_v<std::invoke_result_t<Fn, TriangleList&>>,
                      "MeshData::edit_triangles callback must return void");
        std::invoke(std::forward<Fn>(fn), triangles_);
        mark_geometry_dirty();
        recompute_bounds();
    }

    template <typename Fn> void edit_triangles_without_recompute_bounds(Fn&& fn) {
        static_assert(std::is_void_v<std::invoke_result_t<Fn, TriangleList&>>,
                      "MeshData::edit_triangles_without_recompute_bounds callback must return "
                      "void");
        std::invoke(std::forward<Fn>(fn), triangles_);
        mark_geometry_dirty();
    }

    void set_triangles(TriangleList triangles) {
        triangles_ = std::move(triangles);
        mark_geometry_dirty();
        recompute_bounds();
    }

    void append_triangle(Triangle triangle) {
        triangles_.push_back(triangle);
        mark_geometry_dirty();
    }

    void clear_triangles() {
        triangles_.clear();
        mark_geometry_dirty();
        local_bounds_ = BoundingSphere{};
    }

    void set_geometry_revision_for_testing(std::uint64_t revision) {
        geometry_revision_ = revision;
    }

    void recompute_bounds();

  private:
    void mark_geometry_dirty() {
        ++geometry_revision_;
        if (geometry_revision_ == 0U) {
            ++geometry_revision_;
        }
    }

    TriangleList triangles_;
    SkinnedVertexList skinned_vertices_;
    IndexList skinned_indices_;
    std::vector<Mat4> skin_palette_;
    BoundingSphere local_bounds_{};
    std::uint64_t geometry_revision_ = 1U;
};

enum class MaterialBlendMode {
    Opaque = 0,
    AlphaBlend,
};

enum class MaterialCullMode {
    Clockwise = 0,
    CounterClockwise,
    Disabled,
};

struct Material {
    std::string shader_name = "mesh";
    Color3 base_color = kDefaultObjectColor;
    float base_alpha = 1.0F;
    float alpha_cutoff = -1.0F;
    MaterialBlendMode blend_mode = MaterialBlendMode::Opaque;
    MaterialCullMode cull_mode = MaterialCullMode::Clockwise;
    std::string albedo_texture_path;
};

struct RenderObject {
    std::size_t mesh_id = 0;
    Transform transform;
    std::size_t material_id = 0;
    bool visible = true;
};

class RenderWorld {
  public:
    [[nodiscard]] const AmbientLight& ambient_light() const;
    [[nodiscard]] const DirectionalLight& directional_light() const;
    void set_ambient_light(const AmbientLight& light);
    void set_directional_light(const DirectionalLight& light);

    [[nodiscard]] float sim_time_seconds() const;
    void set_sim_time_seconds(float sim_time_seconds);

    std::vector<MeshData>& meshes();
    [[nodiscard]] const std::vector<MeshData>& meshes() const;
    std::vector<Material>& materials();
    [[nodiscard]] const std::vector<Material>& materials() const;
    std::vector<RenderObject>& objects();
    [[nodiscard]] const std::vector<RenderObject>& objects() const;

  private:
    AmbientLight ambient_light_{};
    DirectionalLight directional_light_{};
    std::vector<MeshData> meshes_;
    std::vector<Material> materials_;
    std::vector<RenderObject> objects_;
    float sim_time_seconds_ = 0.0F;
};

} // namespace isla::client
