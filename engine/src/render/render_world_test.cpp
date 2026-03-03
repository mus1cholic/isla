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

TEST(RenderWorldMeshDataTest, SetSkinnedGeometryAndPaletteStoresData) {
    MeshData mesh;
    const std::uint64_t initial_revision = mesh.geometry_revision();

    mesh.set_skinned_geometry(
        {
            SkinnedMeshVertex{
                .position = Vec3{ .x = 1.0F, .y = 2.0F, .z = 3.0F },
                .normal = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .uv = Vec2{ .x = 0.25F, .y = 0.75F },
                .joints = { 0U, 1U, 0U, 0U },
                .weights = { 0.75F, 0.25F, 0.0F, 0.0F },
            },
        },
        { 0U });
    mesh.set_skin_palette({ Mat4::identity() });

    EXPECT_TRUE(mesh.has_skinned_geometry());
    ASSERT_EQ(mesh.skinned_vertices().size(), 1U);
    ASSERT_EQ(mesh.skinned_indices().size(), 1U);
    EXPECT_EQ(mesh.skinned_indices()[0], 0U);
    EXPECT_EQ(mesh.skin_palette().size(), 1U);
    EXPECT_EQ(mesh.geometry_revision(), initial_revision + 1U);
}

} // namespace
} // namespace isla::client
