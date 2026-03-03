#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "shared/src/test_runfiles.hpp"

namespace isla::client::mesh_asset_loader {
namespace {

std::filesystem::path make_unique_temp_dir() {
    const auto base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 100; ++i) {
        const auto candidate = base / ("isla_mesh_loader_test_" + std::to_string(i) + "_" +
                                       std::to_string(std::rand()));
        std::error_code ec;
        if (std::filesystem::create_directories(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
}

} // namespace

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

TEST(MeshAssetLoaderTests, LoadsAllTrianglePrimitivesAcrossMeshes) {
    const std::filesystem::path temp_dir = make_unique_temp_dir();
    ASSERT_FALSE(temp_dir.empty());
    const std::filesystem::path gltf_path = temp_dir / "multi_mesh_two_triangles.gltf";

    constexpr char kMultiMeshGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"meshes\":["
        "{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"mode\":4}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":1},\"mode\":4}]}"
        "]"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kMultiMeshGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.triangles.size(), 2U);
    EXPECT_FLOAT_EQ(loaded.triangles[0].a.z, 0.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[1].a.z, 1.0F);
}

} // namespace isla::client::mesh_asset_loader
