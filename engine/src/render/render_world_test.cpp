#include "isla/engine/render/render_world.hpp"

#include <gtest/gtest.h>

namespace isla::client {
namespace {

TEST(RenderWorldMeshDataTest, EditWithoutBoundsRecomputeKeepsBoundsStaleUntilManualRecompute) {
    MeshData mesh;
    mesh.set_triangles({
        Triangle{
            .a = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .b = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .c = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
        },
    });

    const BoundingSphere initial_bounds = mesh.local_bounds();
    const std::uint64_t initial_revision = mesh.geometry_revision();

    mesh.edit_triangles_without_recompute_bounds([](std::vector<Triangle>& triangles) {
        triangles.at(0).a.x += 5.0F;
        triangles.at(0).b.x += 5.0F;
        triangles.at(0).c.x += 5.0F;
    });

    EXPECT_EQ(mesh.geometry_revision(), initial_revision + 1U);
    EXPECT_FLOAT_EQ(mesh.local_bounds().center.x, initial_bounds.center.x);
    EXPECT_FLOAT_EQ(mesh.local_bounds().center.y, initial_bounds.center.y);
    EXPECT_FLOAT_EQ(mesh.local_bounds().center.z, initial_bounds.center.z);
    EXPECT_FLOAT_EQ(mesh.local_bounds().radius, initial_bounds.radius);

    mesh.recompute_bounds();
    EXPECT_GT(mesh.local_bounds().center.x, initial_bounds.center.x + 1.0F);
}

} // namespace
} // namespace isla::client
