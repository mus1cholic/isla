#include "isla/engine/render/render_world.hpp"

#include <algorithm>
#include <cmath>

namespace isla::client {

void MeshData::recompute_bounds() {
    if (triangles_.empty()) {
        local_bounds_ = BoundingSphere{};
        return;
    }

    Vec3 min_pt = triangles_[0].a;
    Vec3 max_pt = triangles_[0].a;

    auto add_pt = [&](const Vec3& p) {
        min_pt.x = std::min(min_pt.x, p.x);
        min_pt.y = std::min(min_pt.y, p.y);
        min_pt.z = std::min(min_pt.z, p.z);
        max_pt.x = std::max(max_pt.x, p.x);
        max_pt.y = std::max(max_pt.y, p.y);
        max_pt.z = std::max(max_pt.z, p.z);
    };

    for (const auto& tri : triangles_) {
        add_pt(tri.a);
        add_pt(tri.b);
        add_pt(tri.c);
    }

    const Vec3 center = { .x = (min_pt.x + max_pt.x) * 0.5F,
                          .y = (min_pt.y + max_pt.y) * 0.5F,
                          .z = (min_pt.z + max_pt.z) * 0.5F };

    float max_sq = 0.0F;
    auto check_pt = [&](const Vec3& p) {
        const float dx = p.x - center.x;
        const float dy = p.y - center.y;
        const float dz = p.z - center.z;
        max_sq = std::max(max_sq, (dx * dx) + (dy * dy) + (dz * dz));
    };

    for (const auto& tri : triangles_) {
        check_pt(tri.a);
        check_pt(tri.b);
        check_pt(tri.c);
    }

    local_bounds_ = BoundingSphere{ .center = center, .radius = std::sqrt(max_sq) };
}

const AmbientLight& RenderWorld::ambient_light() const {
    return ambient_light_;
}

const DirectionalLight& RenderWorld::directional_light() const {
    return directional_light_;
}

void RenderWorld::set_ambient_light(const AmbientLight& light) {
    ambient_light_ = light;
}

void RenderWorld::set_directional_light(const DirectionalLight& light) {
    directional_light_ = light;
}

float RenderWorld::sim_time_seconds() const {
    return sim_time_seconds_;
}

void RenderWorld::set_sim_time_seconds(float sim_time_seconds) {
    if (!std::isfinite(sim_time_seconds) || sim_time_seconds < 0.0F) {
        sim_time_seconds_ = 0.0F;
        return;
    }
    sim_time_seconds_ = sim_time_seconds;
}

std::vector<MeshData>& RenderWorld::meshes() {
    return meshes_;
}

const std::vector<MeshData>& RenderWorld::meshes() const {
    return meshes_;
}

std::vector<Material>& RenderWorld::materials() {
    return materials_;
}

const std::vector<Material>& RenderWorld::materials() const {
    return materials_;
}

std::vector<RenderObject>& RenderWorld::objects() {
    return objects_;
}

const std::vector<RenderObject>& RenderWorld::objects() const {
    return objects_;
}

} // namespace isla::client


