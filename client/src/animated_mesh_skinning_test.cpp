#include "animated_mesh_skinning.hpp"

#include <gtest/gtest.h>

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

    const std::vector<Triangle> triangles = make_triangles_from_skinned_primitive(primitive, &skin_matrices);
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

    const std::vector<Triangle> triangles = make_triangles_from_skinned_primitive(primitive, &skin_matrices);
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

    const std::vector<Triangle> triangles = make_triangles_from_skinned_primitive(primitive, &skin_matrices);
    ASSERT_EQ(triangles.size(), 1U);
    EXPECT_NEAR(triangles[0].a.x, 3.0F, 1.0e-4F);
}

TEST(AnimatedMeshSkinningTest, SkipsIncompleteOrInvalidTrianglesSafely) {
    SkinnedPrimitive primitive;
    primitive.vertices.resize(2U);
    primitive.indices = { 0U, 1U, 5U, 0U, 1U };

    const std::vector<Triangle> triangles = make_triangles_from_skinned_primitive(primitive, nullptr);
    EXPECT_TRUE(triangles.empty());
}

} // namespace
} // namespace isla::client::animated_mesh_skinning
