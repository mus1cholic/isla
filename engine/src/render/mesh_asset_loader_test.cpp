#include <gtest/gtest.h>

#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "shared/src/test_runfiles.hpp"

namespace isla::client::mesh_asset_loader {

TEST(MeshAssetLoaderTests, LoadsObjAndTriangulatesFace) {
    const MeshAssetLoadResult loaded = load_from_file(
        isla::shared::test::runfile_path("engine/src/render/testdata/textured_quad.obj"));
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.triangles.size(), 2U);

    EXPECT_FLOAT_EQ(loaded.triangles[0].a.x, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].a.y, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].a.z, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].uv_a.x, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].uv_a.y, 0.0F);

    EXPECT_FLOAT_EQ(loaded.triangles[1].b.x, 1.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[1].b.y, 1.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[1].b.z, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[1].uv_b.x, 1.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[1].uv_b.y, 1.0F);
}

TEST(MeshAssetLoaderTests, LoadsEmbeddedGltfTriangle) {
    const MeshAssetLoadResult loaded = load_from_file(
        isla::shared::test::runfile_path("engine/src/render/testdata/triangle_embedded.gltf"));
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.triangles.size(), 1U);

    const Triangle& triangle = loaded.triangles[0];
    EXPECT_FLOAT_EQ(triangle.a.x, 0.0F);
    EXPECT_FLOAT_EQ(triangle.a.y, 0.0F);
    EXPECT_FLOAT_EQ(triangle.a.z, 0.0F);
    EXPECT_FLOAT_EQ(triangle.b.x, 1.0F);
    EXPECT_FLOAT_EQ(triangle.b.y, 0.0F);
    EXPECT_FLOAT_EQ(triangle.b.z, 0.0F);
    EXPECT_FLOAT_EQ(triangle.c.x, 0.0F);
    EXPECT_FLOAT_EQ(triangle.c.y, 1.0F);
    EXPECT_FLOAT_EQ(triangle.c.z, 0.0F);

    EXPECT_FLOAT_EQ(triangle.uv_a.x, 0.0F);
    EXPECT_FLOAT_EQ(triangle.uv_a.y, 0.0F);
    EXPECT_FLOAT_EQ(triangle.uv_b.x, 1.0F);
    EXPECT_FLOAT_EQ(triangle.uv_b.y, 0.0F);
    EXPECT_FLOAT_EQ(triangle.uv_c.x, 0.0F);
    EXPECT_FLOAT_EQ(triangle.uv_c.y, 1.0F);
}

TEST(MeshAssetLoaderTests, UnsupportedExtensionReturnsError) {
    const MeshAssetLoadResult loaded = load_from_file("engine/src/render/testdata/unknown.mesh");
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.error_message.empty());
}

} // namespace isla::client::mesh_asset_loader
