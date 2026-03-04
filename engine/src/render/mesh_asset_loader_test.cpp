#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "shared/src/test_runfiles.hpp"

namespace isla::client::mesh_asset_loader {
namespace {

class ScopedCurrentPath {
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& path) {
        std::error_code ec;
        original_ = std::filesystem::current_path(ec);
        if (ec) {
            return;
        }
        std::filesystem::current_path(path, ec);
        if (ec) {
            original_.clear();
            return;
        }
        armed_ = true;
    }

    ~ScopedCurrentPath() {
        if (!armed_) {
            return;
        }
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

    [[nodiscard]] bool is_armed() const {
        return armed_;
    }

  private:
    std::filesystem::path original_;
    bool armed_ = false;
};

class ScopedTempDir {
  public:
    static ScopedTempDir create(std::string_view prefix) {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return {};
        }

        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<std::uint64_t> distribution;
        for (int i = 0; i < 100; ++i) {
            const auto candidate =
                base / (std::string(prefix) + "_" + std::to_string(distribution(rng)));
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                return ScopedTempDir(candidate);
            }
            ec.clear();
        }
        return {};
    }

    ScopedTempDir() = default;
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = default;
    ScopedTempDir& operator=(ScopedTempDir&&) = default;

    ~ScopedTempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] bool is_valid() const {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    explicit ScopedTempDir(std::filesystem::path path) : path_(std::move(path)) {}

    std::filesystem::path path_{};
};

} // namespace

TEST(MeshAssetLoaderTests, LoadsObjAndTriangulatesFace) {
    const MeshAssetLoadResult loaded = load_from_file(
        isla::shared::test::runfile_path("engine/src/render/testdata/textured_quad.obj"));
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.primitives.size(), 1U);
    ASSERT_EQ(loaded.triangles.size(), 2U);
    ASSERT_EQ(loaded.primitives[0].triangles.size(), loaded.triangles.size());

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
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "multi_mesh_two_triangles.gltf";

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

TEST(MeshAssetLoaderTests, LoadsGltfNormalsAndMaterialInputs) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "triangle_normals_material.gltf";
    const std::filesystem::path texture_path = temp_dir.path() / "albedo.png";
    {
        std::ofstream texture_stream(texture_path, std::ios::binary);
        ASSERT_TRUE(texture_stream.is_open());
        texture_stream << "not_a_real_png";
    }

    constexpr char kTriangleNormalsMaterialGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"
        "AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/\","
        "\"byteLength\":96}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}"
        "],"
        "\"images\":[{\"uri\":\"albedo.png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{"
        "\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.2,0.4,0.6,0.5],"
        "\"baseColorTexture\":{\"index\":0}"
        "},"
        "\"doubleSided\":true,"
        "\"alphaMode\":\"BLEND\""
        "}],"
        "\"meshes\":[{"
        "\"primitives\":[{"
        "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"material\":0,"
        "\"mode\":4"
        "}]"
        "}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kTriangleNormalsMaterialGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.triangles.size(), 1U);
    EXPECT_TRUE(loaded.triangles[0].has_vertex_normals);
    EXPECT_FLOAT_EQ(loaded.triangles[0].normal_a.z, 1.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].normal_b.z, 1.0F);
    EXPECT_FLOAT_EQ(loaded.triangles[0].normal_c.z, 1.0F);
    EXPECT_NEAR(loaded.material.base_color.r, 0.2F, 1.0e-6F);
    EXPECT_NEAR(loaded.material.base_color.g, 0.4F, 1.0e-6F);
    EXPECT_NEAR(loaded.material.base_color.b, 0.6F, 1.0e-6F);
    EXPECT_NEAR(loaded.material.base_alpha, 0.5F, 1.0e-6F);
    EXPECT_LT(loaded.material.alpha_cutoff, 0.0F);
    EXPECT_EQ(loaded.material.blend_mode, MaterialBlendMode::AlphaBlend);
    EXPECT_EQ(loaded.material.cull_mode, MaterialCullMode::Disabled);
    EXPECT_EQ(loaded.material.albedo_texture_path, texture_path.lexically_normal().string());
}

TEST(MeshAssetLoaderTests, LoadsGltfMaskAlphaCutoffMaterial) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "triangle_mask_material.gltf";

    constexpr char kTriangleMaskMaterialGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAA\",\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"materials\":[{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.42}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4}]}"
        "],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kTriangleMaskMaterialGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    EXPECT_NEAR(loaded.material.alpha_cutoff, 0.42F, 1.0e-6F);
    EXPECT_EQ(loaded.material.blend_mode, MaterialBlendMode::Opaque);
}

TEST(MeshAssetLoaderTests, PreservesPerPrimitiveMaterialMappingForMultiPrimitiveGltf) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "multi_primitive_materials.gltf";

    constexpr char kMultiPrimitiveMaterialGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"materials\":["
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,0.75,0.9]},\"alphaMode\":"
        "\"BLEND\"},"
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0]}}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kMultiPrimitiveMaterialGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.primitives.size(), 2U);
    ASSERT_EQ(loaded.triangles.size(), 2U);
    EXPECT_EQ(loaded.primitives[0].triangles.size(), 1U);
    EXPECT_EQ(loaded.primitives[1].triangles.size(), 1U);
    EXPECT_NEAR(loaded.primitives[0].material.base_color.r, 0.25F, 1.0e-6F);
    EXPECT_NEAR(loaded.primitives[0].material.base_color.g, 0.5F, 1.0e-6F);
    EXPECT_NEAR(loaded.primitives[0].material.base_color.b, 0.75F, 1.0e-6F);
    EXPECT_NEAR(loaded.primitives[0].material.base_alpha, 0.9F, 1.0e-6F);
    EXPECT_EQ(loaded.primitives[0].material.blend_mode, MaterialBlendMode::AlphaBlend);
    EXPECT_NEAR(loaded.primitives[1].material.base_color.r, 1.0F, 1.0e-6F);
    EXPECT_NEAR(loaded.primitives[1].material.base_color.g, 0.0F, 1.0e-6F);
    EXPECT_NEAR(loaded.primitives[1].material.base_color.b, 0.0F, 1.0e-6F);
    EXPECT_EQ(loaded.primitives[1].material.blend_mode, MaterialBlendMode::Opaque);
}

TEST(MeshAssetLoaderTests, ObjLoadResultPrimitivesCompatibilityMatchesLegacyTriangles) {
    const MeshAssetLoadResult loaded = load_from_file(
        isla::shared::test::runfile_path("engine/src/render/testdata/textured_quad.obj"));
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.primitives.size(), 1U);
    ASSERT_EQ(loaded.primitives[0].triangles.size(), loaded.triangles.size());
    for (std::size_t i = 0U; i < loaded.triangles.size(); ++i) {
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].a.x, loaded.triangles[i].a.x);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].a.y, loaded.triangles[i].a.y);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].a.z, loaded.triangles[i].a.z);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].b.x, loaded.triangles[i].b.x);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].b.y, loaded.triangles[i].b.y);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].b.z, loaded.triangles[i].b.z);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].c.x, loaded.triangles[i].c.x);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].c.y, loaded.triangles[i].c.y);
        EXPECT_FLOAT_EQ(loaded.primitives[0].triangles[i].c.z, loaded.triangles[i].c.z);
    }
}

TEST(MeshAssetLoaderTests, MultiPrimitiveMaterialMappingIsDeterministicAcrossLoads) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "multi_primitive_deterministic.gltf";

    constexpr char kMultiPrimitiveMaterialGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"materials\":["
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.1,0.2,0.3,1.0]}},"
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.8,0.7,1.0]},\"alphaMode\":"
        "\"BLEND\"}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kMultiPrimitiveMaterialGltf;
    }

    const MeshAssetLoadResult first = load_from_file(gltf_path.string());
    const MeshAssetLoadResult second = load_from_file(gltf_path.string());
    ASSERT_TRUE(first.ok) << first.error_message;
    ASSERT_TRUE(second.ok) << second.error_message;
    ASSERT_EQ(first.primitives.size(), second.primitives.size());
    ASSERT_EQ(first.primitives.size(), 2U);

    for (std::size_t i = 0U; i < first.primitives.size(); ++i) {
        ASSERT_EQ(first.primitives[i].triangles.size(), second.primitives[i].triangles.size());
        EXPECT_FLOAT_EQ(first.primitives[i].material.base_color.r,
                        second.primitives[i].material.base_color.r);
        EXPECT_FLOAT_EQ(first.primitives[i].material.base_color.g,
                        second.primitives[i].material.base_color.g);
        EXPECT_FLOAT_EQ(first.primitives[i].material.base_color.b,
                        second.primitives[i].material.base_color.b);
        EXPECT_FLOAT_EQ(first.primitives[i].material.base_alpha,
                        second.primitives[i].material.base_alpha);
        EXPECT_EQ(first.primitives[i].material.blend_mode,
                  second.primitives[i].material.blend_mode);
        EXPECT_EQ(first.primitives[i].material.cull_mode, second.primitives[i].material.cull_mode);
    }
}

TEST(MeshAssetLoaderTests, PreservesDefaultMaterialOnlyForPrimitiveWithoutMaterial) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "mixed_material_presence.gltf";

    constexpr char kMixedMaterialPresenceGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.1,0.2,0.8]},"
        "\"alphaMode\":\"BLEND\",\"doubleSided\":true}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":0,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kMixedMaterialPresenceGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.primitives.size(), 2U);
    EXPECT_EQ(loaded.primitives[0].triangles.size(), 1U);
    EXPECT_EQ(loaded.primitives[1].triangles.size(), 1U);

    const MeshAssetMaterial& default_material = loaded.primitives[0].material;
    EXPECT_FLOAT_EQ(default_material.base_color.r, 1.0F);
    EXPECT_FLOAT_EQ(default_material.base_color.g, 1.0F);
    EXPECT_FLOAT_EQ(default_material.base_color.b, 1.0F);
    EXPECT_FLOAT_EQ(default_material.base_alpha, 1.0F);
    EXPECT_LT(default_material.alpha_cutoff, 0.0F);
    EXPECT_EQ(default_material.blend_mode, MaterialBlendMode::Opaque);
    EXPECT_EQ(default_material.cull_mode, MaterialCullMode::CounterClockwise);
    EXPECT_TRUE(default_material.albedo_texture_path.empty());

    const MeshAssetMaterial& explicit_material = loaded.primitives[1].material;
    EXPECT_NEAR(explicit_material.base_color.r, 0.9F, 1.0e-6F);
    EXPECT_NEAR(explicit_material.base_color.g, 0.1F, 1.0e-6F);
    EXPECT_NEAR(explicit_material.base_color.b, 0.2F, 1.0e-6F);
    EXPECT_NEAR(explicit_material.base_alpha, 0.8F, 1.0e-6F);
    EXPECT_EQ(explicit_material.blend_mode, MaterialBlendMode::AlphaBlend);
    EXPECT_EQ(explicit_material.cull_mode, MaterialCullMode::Disabled);
}

TEST(MeshAssetLoaderTests, RejectsAbsoluteImageUriPath) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "triangle_absolute_uri.gltf";

    constexpr char kTriangleAbsoluteUriGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAA\",\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"images\":[{\"uri\":\"C:/Windows/win.ini\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4}]}"
        "],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kTriangleAbsoluteUriGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    EXPECT_TRUE(loaded.material.albedo_texture_path.empty());
}

TEST(MeshAssetLoaderTests, RejectsImageUriParentTraversalOutsideAssetDirectory) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path nested_dir = temp_dir.path() / "nested";
    ASSERT_TRUE(std::filesystem::create_directories(nested_dir));
    const std::filesystem::path gltf_path = nested_dir / "triangle_parent_traversal_uri.gltf";

    constexpr char kTriangleParentTraversalUriGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAA\",\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"images\":[{\"uri\":\"../outside.png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4}]}"
        "],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kTriangleParentTraversalUriGltf;
    }

    const MeshAssetLoadResult loaded = load_from_file(gltf_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    EXPECT_TRUE(loaded.material.albedo_texture_path.empty());
}

TEST(MeshAssetLoaderTests, RejectsImageUriParentTraversalForParentlessRelativeAssetPath) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_mesh_loader_test");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = temp_dir.path() / "model.gltf";

    constexpr char kParentlessTraversalUriGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAA\",\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"images\":[{\"uri\":\"../outside.png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4}]}"
        "],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    {
        std::ofstream stream(gltf_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << kParentlessTraversalUriGltf;
    }

    ScopedCurrentPath cwd_guard(temp_dir.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    const MeshAssetLoadResult loaded = load_from_file("model.gltf");
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    EXPECT_TRUE(loaded.material.albedo_texture_path.empty());
}

} // namespace isla::client::mesh_asset_loader
