#include "engine/src/render/include/pmx_physics_sidecar.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace isla::client::pmx_physics_sidecar {
namespace {

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
        for (int i = 0; i < 64; ++i) {
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

std::filesystem::path write_sidecar(ScopedTempDir& temp_dir, std::string_view filename,
                                    std::string_view json) {
    const std::filesystem::path sidecar_path = temp_dir.path() / std::string(filename);
    std::ofstream out(sidecar_path, std::ios::binary);
    if (!out.is_open()) {
        return {};
    }
    out << json;
    if (!out.good()) {
        return {};
    }
    return sidecar_path;
}

TEST(PmxPhysicsSidecarTest, LoadsValidSidecarAndFiltersUnknownBones) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = temp_dir.path() / "model.physics.json";
    constexpr char kSidecarJson[] =
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"collision_layers\":[{\"index\":0,\"name\":\"default\"}],"
        "\"colliders\":["
        "{\"id\":\"c0\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1,\"radius\":"
        "0.2},"
        "{\"id\":\"c1\",\"bone_name\":\"Unknown\",\"shape\":\"box\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1,\"size\":[1,"
        "1,1]}"
        "],"
        "\"constraints\":[{\"id\":\"k0\",\"bone_a_name\":\"Head\",\"bone_b_name\":\"Neck\","
        "\"type\":\"cone_twist\"}]"
        "}";
    {
        std::ofstream out(sidecar_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << kSidecarJson;
    }

    const std::vector<std::string> known_joints{ "Head", "Neck" };
    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), known_joints);
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    EXPECT_EQ(loaded.sidecar.collision_layers.size(), 1U);
    EXPECT_EQ(loaded.sidecar.colliders.size(), 1U);
    EXPECT_EQ(loaded.sidecar.colliders[0].id, "c0");
    EXPECT_EQ(loaded.sidecar.colliders[0].shape, ColliderShape::Sphere);
    EXPECT_EQ(loaded.sidecar.constraints.size(), 1U);
    EXPECT_FALSE(loaded.warnings.empty());
}

TEST(PmxPhysicsSidecarTest, FailsOnSchemaVersionMismatch) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = temp_dir.path() / "bad.physics.json";
    constexpr char kSidecarJson[] =
        "{"
        "\"schema_version\":\"1.0.1\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"collision_layers\":[],"
        "\"colliders\":[],"
        "\"constraints\":[]"
        "}";
    {
        std::ofstream out(sidecar_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << kSidecarJson;
    }

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.error_message.empty());
}

TEST(PmxPhysicsSidecarTest, FailsOnInvalidJsonSyntax) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path =
        write_sidecar(temp_dir, "invalid.physics.json", "{\"schema_version\":");
    ASSERT_FALSE(sidecar_path.empty());

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error_message.find("failed to parse physics sidecar JSON"), std::string::npos);
}

TEST(PmxPhysicsSidecarTest, FailsWhenRequiredTopLevelFieldsMissing) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = write_sidecar(
        temp_dir, "missing_fields.physics.json",
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"colliders\":[],"
        "\"constraints\":[]"
        "}");
    ASSERT_FALSE(sidecar_path.empty());

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string());
    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.error_message, "physics sidecar missing required top-level arrays");
}

TEST(PmxPhysicsSidecarTest, SkipsInvalidShapeParametersAndRetainsValidColliders) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = write_sidecar(
        temp_dir, "invalid_shapes.physics.json",
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"collision_layers\":[{\"index\":0,\"name\":\"default\"}],"
        "\"colliders\":["
        "{\"id\":\"bad_sphere\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1},"
        "{\"id\":\"bad_capsule\",\"bone_name\":\"Head\",\"shape\":\"capsule\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1,\"radius\":"
        "0.1},"
        "{\"id\":\"bad_box\",\"bone_name\":\"Head\",\"shape\":\"box\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1,\"size\":[1,"
        "0,1]},"
        "{\"id\":\"good_box\",\"bone_name\":\"Head\",\"shape\":\"box\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":1,\"size\":[1,"
        "1,1]}"
        "],"
        "\"constraints\":[]"
        "}");
    ASSERT_FALSE(sidecar_path.empty());

    const std::vector<std::string> known_joints{ "Head" };
    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), known_joints);
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.colliders.size(), 1U);
    EXPECT_EQ(loaded.sidecar.colliders[0].id, "good_box");
    EXPECT_FALSE(loaded.warnings.empty());
}

TEST(PmxPhysicsSidecarTest, SkipsInvalidLayerMaskNumericBounds) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = write_sidecar(
        temp_dir, "invalid_layer_mask.physics.json",
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"collision_layers\":[{\"index\":0,\"name\":\"default\"}],"
        "\"colliders\":["
        "{\"id\":\"bad1\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":-1,\"mask\":1,\"radius\":"
        "0.1},"
        "{\"id\":\"bad2\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1.5,\"mask\":1,\"radius\":"
        "0.1},"
        "{\"id\":\"bad3\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":4294967296,"
        "\"radius\":0.1},"
        "{\"id\":\"good\",\"bone_name\":\"Head\",\"shape\":\"sphere\",\"offset\":[0,0,0],"
        "\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,\"layer\":1,\"mask\":2,\"radius\":"
        "0.2}"
        "],"
        "\"constraints\":[]"
        "}");
    ASSERT_FALSE(sidecar_path.empty());

    const std::vector<std::string> known_joints{ "Head" };
    const SidecarLoadResult loaded = load_from_file(sidecar_path.string(), known_joints);
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.colliders.size(), 1U);
    EXPECT_EQ(loaded.sidecar.colliders[0].id, "good");
}

TEST(PmxPhysicsSidecarTest, UnsupportedConstraintTypeWarnsAndSkips) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_pmx_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path sidecar_path = write_sidecar(
        temp_dir, "unsupported_constraint.physics.json",
        "{"
        "\"schema_version\":\"1.0.0\","
        "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\",\"timestamp_utc\":"
        "\"2026-03-01T00:00:00Z\"},"
        "\"collision_layers\":[{\"index\":0,\"name\":\"default\"}],"
        "\"colliders\":[],"
        "\"constraints\":["
        "{\"id\":\"bad\",\"bone_a_name\":\"A\",\"bone_b_name\":\"B\",\"type\":\"spring\"},"
        "{\"id\":\"good\",\"bone_a_name\":\"A\",\"bone_b_name\":\"B\",\"type\":\"hinge\"}"
        "]"
        "}");
    ASSERT_FALSE(sidecar_path.empty());

    const SidecarLoadResult loaded = load_from_file(sidecar_path.string());
    ASSERT_TRUE(loaded.ok) << loaded.error_message;
    ASSERT_EQ(loaded.sidecar.constraints.size(), 1U);
    EXPECT_EQ(loaded.sidecar.constraints[0].id, "good");
    EXPECT_FALSE(loaded.warnings.empty());
}

} // namespace
} // namespace isla::client::pmx_physics_sidecar
