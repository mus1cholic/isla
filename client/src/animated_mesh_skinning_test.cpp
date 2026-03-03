#include "animated_mesh_skinning.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace isla::client::animated_mesh_skinning {
namespace {

using animated_gltf::SkinnedPrimitive;
using animated_gltf::SkinnedVertex;

TEST(AnimatedMeshSkinningTest, BlendsTwoJointInfluences) {
    SkinnedPrimitive primitive;
    primitive.vertices = {
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 1U, 0U, 0U },
            .weights = { 0.5F, 0.5F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 1U, 0U, 0U },
            .weights = { 0.5F, 0.5F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .joints = { 0U, 1U, 0U, 0U },
            .weights = { 0.5F, 0.5F, 0.0F, 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };

    const std::vector<Mat4> skin_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
        Mat4::from_position_scale_quat(Vec3{ .x = 4.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };

    const std::vector<Triangle> triangles =
        make_triangles_from_skinned_primitive(primitive, &skin_matrices);
    ASSERT_EQ(triangles.size(), 1U);
    EXPECT_NEAR(triangles[0].a.x, 3.0F, 1.0e-4F);
    EXPECT_NEAR(triangles[0].b.x, 4.0F, 1.0e-4F);
    EXPECT_NEAR(triangles[0].c.x, 3.0F, 1.0e-4F);
}

TEST(AnimatedMeshSkinningTest, OutOfRangeJointFallsBackToOriginalPosition) {
    SkinnedPrimitive primitive;
    primitive.vertices = {
        SkinnedVertex{
            .position = Vec3{ .x = 5.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 8U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };
    const std::vector<Mat4> skin_matrices = { Mat4::identity() };

    const std::vector<Triangle> triangles =
        make_triangles_from_skinned_primitive(primitive, &skin_matrices);
    ASSERT_EQ(triangles.size(), 1U);
    EXPECT_NEAR(triangles[0].a.x, 5.0F, 1.0e-4F);
}

TEST(AnimatedMeshSkinningTest, RenormalizesWhenSomeInfluencesAreInvalid) {
    SkinnedPrimitive primitive;
    primitive.vertices = {
        SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 9U, 0U, 0U },
            .weights = { 0.5F, 0.5F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };
    const std::vector<Mat4> skin_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };

    const std::vector<Triangle> triangles =
        make_triangles_from_skinned_primitive(primitive, &skin_matrices);
    ASSERT_EQ(triangles.size(), 1U);
    EXPECT_NEAR(triangles[0].a.x, 3.0F, 1.0e-4F);
}

TEST(AnimatedMeshSkinningTest, SkipsIncompleteOrInvalidTrianglesSafely) {
    SkinnedPrimitive primitive;
    primitive.vertices.resize(2U);
    primitive.indices = { 0U, 1U, 5U, 0U, 1U };

    const std::vector<Triangle> triangles =
        make_triangles_from_skinned_primitive(primitive, nullptr);
    EXPECT_TRUE(triangles.empty());
}

TEST(AnimatedMeshSkinningTest, WorkspaceBuildSkipsInvalidTrianglesAndKeepsValidTopology) {
    SkinnedPrimitive primitive;
    primitive.vertices = {
        SkinnedVertex{ .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 1.0F, .y = 1.0F, .z = 0.0F } },
    };
    primitive.indices = { 0U, 1U, 2U, 0U, 1U, 9U };

    PrimitiveSkinningWorkspace workspace;
    const std::vector<Triangle> triangles =
        make_initial_triangles_and_workspace(primitive, &workspace);

    ASSERT_EQ(triangles.size(), 1U);
    ASSERT_EQ(workspace.triangle_vertex_indices.size(), 3U);
    EXPECT_EQ(workspace.triangle_vertex_indices[0], 0U);
    EXPECT_EQ(workspace.triangle_vertex_indices[1], 1U);
    EXPECT_EQ(workspace.triangle_vertex_indices[2], 2U);
}

TEST(AnimatedMeshSkinningTest, InPlaceSkinningReusesTriangleStorageAcrossTicks) {
    SkinnedPrimitive primitive;
    primitive.vertices = {
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };

    PrimitiveSkinningWorkspace workspace;
    std::vector<Triangle> triangles = make_initial_triangles_and_workspace(primitive, &workspace);
    ASSERT_EQ(triangles.size(), 1U);
    const Triangle* original_data = triangles.data();
    const std::size_t original_capacity = triangles.capacity();

    const std::vector<Mat4> tick0_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };
    skin_primitive_in_place(primitive, &tick0_matrices, &workspace, &triangles);
    EXPECT_NEAR(triangles[0].a.x, 1.0F, 1.0e-4F);

    const std::vector<Mat4> tick1_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 3.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };
    skin_primitive_in_place(primitive, &tick1_matrices, &workspace, &triangles);
    EXPECT_NEAR(triangles[0].a.x, 3.0F, 1.0e-4F);
    EXPECT_EQ(triangles.data(), original_data);
    EXPECT_EQ(triangles.capacity(), original_capacity);
}

TEST(AnimatedMeshSkinningTest, InPlaceSkinningKeepsWorkspaceAndTriangleCapacityStable) {
    SkinnedPrimitive primitive;
    primitive.vertices.reserve(96U);
    for (std::uint32_t i = 0U; i < 96U; ++i) {
        primitive.vertices.push_back(SkinnedVertex{
            .position = Vec3{ .x = static_cast<float>(i), .y = static_cast<float>(i % 7U), .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        });
    }
    for (std::uint32_t i = 0U; i + 2U < 96U; i += 3U) {
        primitive.indices.push_back(i);
        primitive.indices.push_back(i + 1U);
        primitive.indices.push_back(i + 2U);
    }

    PrimitiveSkinningWorkspace workspace;
    std::vector<Triangle> triangles = make_initial_triangles_and_workspace(primitive, &workspace);
    ASSERT_FALSE(triangles.empty());
    const std::vector<Mat4> skin_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 0.25F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };

    skin_primitive_in_place(primitive, &skin_matrices, &workspace, &triangles);
    const Triangle* triangle_data = triangles.data();
    const std::size_t triangle_capacity = triangles.capacity();
    const std::size_t skinned_positions_capacity = workspace.skinned_positions.capacity();
    const std::size_t topology_capacity = workspace.triangle_vertex_indices.capacity();

    for (int tick = 0; tick < 120; ++tick) {
        skin_primitive_in_place(primitive, &skin_matrices, &workspace, &triangles);
        EXPECT_EQ(triangles.data(), triangle_data);
        EXPECT_EQ(triangles.capacity(), triangle_capacity);
        EXPECT_EQ(workspace.skinned_positions.capacity(), skinned_positions_capacity);
        EXPECT_EQ(workspace.triangle_vertex_indices.capacity(), topology_capacity);
    }
}

TEST(AnimatedMeshSkinningTest, InPlaceSkinningRebuildsWorkspaceWhenTopologyIsStale) {
    SkinnedPrimitive primitive_a;
    primitive_a.vertices = {
        SkinnedVertex{ .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F } },
        SkinnedVertex{ .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F } },
    };
    primitive_a.indices = { 0U, 1U, 3U };

    PrimitiveSkinningWorkspace workspace;
    std::vector<Triangle> triangles =
        make_initial_triangles_and_workspace(primitive_a, &workspace);
    ASSERT_EQ(triangles.size(), 1U);
    ASSERT_EQ(workspace.triangle_vertex_indices.size(), 3U);
    EXPECT_EQ(workspace.triangle_vertex_indices[2], 3U);

    SkinnedPrimitive primitive_b;
    primitive_b.vertices = {
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
    };
    primitive_b.indices = { 0U, 1U, 2U };

    const std::vector<Mat4> skin_matrices = {
        Mat4::from_position_scale_quat(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F },
                                       Vec3{ .x = 1.0F, .y = 1.0F, .z = 1.0F }, Quat::identity()),
    };

    skin_primitive_in_place(primitive_b, &skin_matrices, &workspace, &triangles);
    ASSERT_EQ(triangles.size(), 1U);
    EXPECT_EQ(workspace.triangle_vertex_indices[0], 0U);
    EXPECT_EQ(workspace.triangle_vertex_indices[1], 1U);
    EXPECT_EQ(workspace.triangle_vertex_indices[2], 2U);
    EXPECT_NEAR(triangles[0].a.x, 2.0F, 1.0e-4F);
}

} // namespace
} // namespace isla::client::animated_mesh_skinning
