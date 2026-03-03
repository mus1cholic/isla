#include "engine/src/render/include/model_renderer_skinning_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/math/render_math.hpp"

namespace isla::client {
namespace {

using namespace render_math;

TEST(ModelRendererSkinningUtilsTest, ProgramSelectionFallsBackToStaticWhenAnyGateFails) {
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = false,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = false,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = false,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::StaticMesh);
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = false,
              }),
              SkinningProgramPath::StaticMesh);
}

TEST(ModelRendererSkinningUtilsTest, ProgramSelectionUsesSkinnedWhenAllGatesPass) {
    EXPECT_EQ(choose_skinning_program_path(SkinningProgramDecisionInputs{
                  .mesh_is_skinned = true,
                  .has_skin_palette = true,
                  .gpu_skinning_supported = true,
                  .skinned_program_valid = true,
              }),
              SkinningProgramPath::SkinnedMesh);
}

TEST(ModelRendererSkinningUtilsTest, FillSkinPaletteBufferCopiesAndPadsWithIdentity) {
    std::vector<Mat4> upload(4U, Mat4::translation(Vec3{ .x = 99.0F, .y = 99.0F, .z = 99.0F }));
    const std::vector<Mat4> source = {
        Mat4::translation(Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F }),
        Mat4::translation(Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F }),
    };

    std::size_t copied = 0U;
    const bool truncated = fill_skin_palette_upload_buffer(source, upload, &copied);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(copied, 2U);
    EXPECT_NEAR(upload[0].elements[12], 1.0F, 1.0e-6F);
    EXPECT_NEAR(upload[1].elements[12], 2.0F, 1.0e-6F);
    EXPECT_NEAR(upload[2].elements[12], 0.0F, 1.0e-6F);
    EXPECT_NEAR(upload[3].elements[12], 0.0F, 1.0e-6F);
}

TEST(ModelRendererSkinningUtilsTest, FillSkinPaletteBufferReportsTruncationAtLimitBoundary) {
    std::vector<Mat4> source(kMaxGpuSkinningJoints + 1U, Mat4::identity());
    source[kMaxGpuSkinningJoints - 1U] =
        Mat4::translation(Vec3{ .x = 64.0F, .y = 0.0F, .z = 0.0F });
    source[kMaxGpuSkinningJoints] = Mat4::translation(Vec3{ .x = 65.0F, .y = 0.0F, .z = 0.0F });
    std::vector<Mat4> upload(kMaxGpuSkinningJoints, Mat4::identity());

    std::size_t copied = 0U;
    const bool truncated = fill_skin_palette_upload_buffer(source, upload, &copied);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(copied, kMaxGpuSkinningJoints);
    EXPECT_NEAR(upload[kMaxGpuSkinningJoints - 1U].elements[12], 64.0F, 1.0e-6F);
}

SkinnedMeshVertex make_joint_vertex(std::uint16_t joint_index) {
    return SkinnedMeshVertex{
        .position = Vec3{ .x = static_cast<float>(joint_index), .y = 0.0F, .z = 0.0F },
        .normal = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
        .uv = Vec2{ .x = 0.0F, .y = 0.0F },
        .joints = { joint_index, 0U, 0U, 0U },
        .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
    };
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsSplitsPrimitiveBeyondPaletteLimit) {
    std::vector<SkinnedMeshVertex> vertices;
    vertices.reserve(66U);
    for (std::uint16_t joint = 0U; joint < 66U; ++joint) {
        vertices.push_back(make_joint_vertex(joint));
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(66U);
    for (std::uint32_t i = 0U; i < 66U; ++i) {
        indices.push_back(i);
    }

    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions, &error))
        << error;
    ASSERT_EQ(partitions.size(), 2U);

    for (const GpuSkinningPartition& partition : partitions) {
        ASSERT_LE(partition.global_joint_palette.size(), kMaxGpuSkinningJoints);
        for (const SkinnedMeshVertex& vertex : partition.vertices) {
            EXPECT_LT(vertex.joints[0], kMaxGpuSkinningJoints);
        }
    }
    EXPECT_EQ(partitions[0].global_joint_palette.size(), 63U);
    EXPECT_EQ(partitions[1].global_joint_palette.size(), 3U);
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsAreDeterministic) {
    std::vector<SkinnedMeshVertex> vertices;
    vertices.reserve(66U);
    for (std::uint16_t joint = 0U; joint < 66U; ++joint) {
        vertices.push_back(make_joint_vertex(joint));
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(66U);
    for (std::uint32_t i = 0U; i < 66U; ++i) {
        indices.push_back(i);
    }

    std::vector<GpuSkinningPartition> first;
    std::vector<GpuSkinningPartition> second;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, first, &error))
        << error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, second, &error))
        << error;
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0U; i < first.size(); ++i) {
        const GpuSkinningPartition& a = first[i];
        const GpuSkinningPartition& b = second[i];
        EXPECT_EQ(a.indices, b.indices);
        EXPECT_EQ(a.global_joint_palette, b.global_joint_palette);
        ASSERT_EQ(a.vertices.size(), b.vertices.size());
        for (std::size_t v = 0U; v < a.vertices.size(); ++v) {
            EXPECT_EQ(a.vertices[v].joints, b.vertices[v].joints);
            EXPECT_EQ(a.vertices[v].weights, b.vertices[v].weights);
        }
    }
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsPreservesJointMappingSemantics) {
    const std::vector<SkinnedMeshVertex> vertices = {
        SkinnedMeshVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .normal = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { 10U, 20U, 0U, 0U },
            .weights = { 0.75F, 0.25F, 0.0F, 0.0F },
        },
        SkinnedMeshVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .normal = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv = Vec2{ .x = 1.0F, .y = 0.0F },
            .joints = { 30U, 40U, 0U, 0U },
            .weights = { 0.60F, 0.40F, 0.0F, 0.0F },
        },
        SkinnedMeshVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .normal = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv = Vec2{ .x = 0.0F, .y = 1.0F },
            .joints = { 20U, 50U, 0U, 0U },
            .weights = { 0.25F, 0.75F, 0.0F, 0.0F },
        },
    };
    const std::vector<std::uint32_t> indices = { 0U, 1U, 2U };

    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions, &error))
        << error;
    ASSERT_EQ(partitions.size(), 1U);
    const GpuSkinningPartition& partition = partitions[0];
    ASSERT_EQ(partition.vertices.size(), 3U);

    for (std::size_t vertex_index = 0U; vertex_index < partition.vertices.size(); ++vertex_index) {
        const SkinnedMeshVertex& source_vertex = vertices[vertex_index];
        const SkinnedMeshVertex& remapped_vertex = partition.vertices[vertex_index];
        for (std::size_t influence = 0U; influence < source_vertex.joints.size(); ++influence) {
            if (source_vertex.weights[influence] <= 1.0e-6F) {
                continue;
            }
            const std::uint16_t local_joint = remapped_vertex.joints[influence];
            ASSERT_LT(static_cast<std::size_t>(local_joint), partition.global_joint_palette.size());
            EXPECT_EQ(partition.global_joint_palette[local_joint], source_vertex.joints[influence]);
        }
    }
}

Vec3 skin_position(const SkinnedMeshVertex& vertex, std::span<const Mat4> palette) {
    Vec3 out{};
    for (std::size_t i = 0U; i < vertex.joints.size(); ++i) {
        const float weight = vertex.weights[i];
        if (weight <= 1.0e-6F) {
            continue;
        }
        const std::size_t joint = static_cast<std::size_t>(vertex.joints[i]);
        if (joint >= palette.size()) {
            continue;
        }
        const Vec3 transformed = transform_point(palette[joint], vertex.position);
        out.x += transformed.x * weight;
        out.y += transformed.y * weight;
        out.z += transformed.z * weight;
    }
    return out;
}

Vec3 skin_normal(const SkinnedMeshVertex& vertex, std::span<const Mat4> palette) {
    Vec3 out{};
    for (std::size_t i = 0U; i < vertex.joints.size(); ++i) {
        const float weight = vertex.weights[i];
        if (weight <= 1.0e-6F) {
            continue;
        }
        const std::size_t joint = static_cast<std::size_t>(vertex.joints[i]);
        if (joint >= palette.size()) {
            continue;
        }
        const Vec3 origin =
            transform_point(palette[joint], Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F });
        const Vec3 endpoint = transform_point(palette[joint], vertex.normal);
        const Vec3 transformed = normalize(endpoint - origin);
        out.x += transformed.x * weight;
        out.y += transformed.y * weight;
        out.z += transformed.z * weight;
    }
    return normalize(out);
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsMatchesGlobalSkinningOutput) {
    const std::vector<SkinnedMeshVertex> vertices = {
        SkinnedMeshVertex{
            .position = Vec3{ .x = 0.25F, .y = 0.0F, .z = 0.5F },
            .normal = Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { 70U, 2U, 65U, 0U },
            .weights = { 0.5F, 0.25F, 0.25F, 0.0F },
        },
        SkinnedMeshVertex{
            .position = Vec3{ .x = 1.25F, .y = 0.5F, .z = 0.5F },
            .normal = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .uv = Vec2{ .x = 1.0F, .y = 0.0F },
            .joints = { 2U, 70U, 65U, 0U },
            .weights = { 0.2F, 0.6F, 0.2F, 0.0F },
        },
        SkinnedMeshVertex{
            .position = Vec3{ .x = 0.25F, .y = 1.0F, .z = 0.25F },
            .normal = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 1.0F },
            .joints = { 65U, 70U, 2U, 0U },
            .weights = { 0.7F, 0.1F, 0.2F, 0.0F },
        },
    };
    const std::vector<std::uint32_t> indices = { 0U, 1U, 2U };

    std::vector<Mat4> global_palette(80U, Mat4::identity());
    global_palette[2U] = Mat4::rotation_z(0.25F);
    global_palette[65U] = Mat4::translation(Vec3{ .x = 3.0F, .y = -2.0F, .z = 0.5F });
    global_palette[70U] = multiply(Mat4::translation(Vec3{ .x = -1.0F, .y = 1.5F, .z = 0.0F }),
                                   Mat4::rotation_y(0.5F));

    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions, &error))
        << error;
    ASSERT_EQ(partitions.size(), 1U);
    const GpuSkinningPartition& partition = partitions[0];
    ASSERT_EQ(partition.vertices.size(), vertices.size());

    std::vector<Mat4> local_palette(partition.global_joint_palette.size(), Mat4::identity());
    for (std::size_t local_joint = 0U; local_joint < partition.global_joint_palette.size();
         ++local_joint) {
        local_palette[local_joint] = global_palette[partition.global_joint_palette[local_joint]];
    }

    for (std::size_t vertex_index = 0U; vertex_index < vertices.size(); ++vertex_index) {
        const Vec3 global_pos = skin_position(vertices[vertex_index], global_palette);
        const Vec3 local_pos = skin_position(partition.vertices[vertex_index], local_palette);
        EXPECT_NEAR(global_pos.x, local_pos.x, 1.0e-4F);
        EXPECT_NEAR(global_pos.y, local_pos.y, 1.0e-4F);
        EXPECT_NEAR(global_pos.z, local_pos.z, 1.0e-4F);

        const Vec3 global_normal = skin_normal(vertices[vertex_index], global_palette);
        const Vec3 local_normal = skin_normal(partition.vertices[vertex_index], local_palette);
        EXPECT_NEAR(global_normal.x, local_normal.x, 1.0e-4F);
        EXPECT_NEAR(global_normal.y, local_normal.y, 1.0e-4F);
        EXPECT_NEAR(global_normal.z, local_normal.z, 1.0e-4F);
    }
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsKeepsSinglePartitionWithinBudget) {
    std::vector<SkinnedMeshVertex> vertices;
    for (std::uint16_t joint = 0U; joint < 10U; ++joint) {
        vertices.push_back(make_joint_vertex(static_cast<std::uint16_t>(120U + joint)));
    }
    const std::vector<std::uint32_t> indices = { 0U, 1U, 2U, 2U, 3U, 4U, 4U, 5U, 6U };

    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions, &error))
        << error;
    ASSERT_EQ(partitions.size(), 1U);
    EXPECT_EQ(partitions[0].indices, indices);
    EXPECT_EQ(partitions[0].vertices.size(), 7U);
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsPreservesSharedVertexTopology) {
    const std::vector<SkinnedMeshVertex> vertices = {
        make_joint_vertex(90U),
        make_joint_vertex(91U),
        make_joint_vertex(92U),
        make_joint_vertex(93U),
    };
    const std::vector<std::uint32_t> indices = { 0U, 1U, 2U, 2U, 1U, 3U };

    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    ASSERT_TRUE(
        build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions, &error))
        << error;
    ASSERT_EQ(partitions.size(), 1U);
    ASSERT_EQ(partitions[0].vertices.size(), vertices.size());
    EXPECT_EQ(partitions[0].indices, indices);
    for (std::uint32_t index : partitions[0].indices) {
        EXPECT_LT(static_cast<std::size_t>(index), partitions[0].vertices.size());
    }
}

TEST(ModelRendererSkinningUtilsTest,
     BuildGpuSkinningPartitionsRejectsIncompleteTriangleIndexBuffer) {
    const std::vector<SkinnedMeshVertex> vertices = {
        make_joint_vertex(1U),
        make_joint_vertex(2U),
    };
    const std::vector<std::uint32_t> indices = { 0U, 1U };
    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    EXPECT_FALSE(build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions,
                                               &error));
}

TEST(ModelRendererSkinningUtilsTest, BuildGpuSkinningPartitionsRejectsOutOfRangeTriangleIndex) {
    const std::vector<SkinnedMeshVertex> vertices = {
        make_joint_vertex(1U),
        make_joint_vertex(2U),
        make_joint_vertex(3U),
    };
    const std::vector<std::uint32_t> indices = { 0U, 1U, 9U };
    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    EXPECT_FALSE(build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions,
                                               &error));
}

TEST(ModelRendererSkinningUtilsTest,
     BuildGpuSkinningPartitionsRejectsNonEmptyIndicesWithEmptyVertices) {
    const std::vector<SkinnedMeshVertex> vertices;
    const std::vector<std::uint32_t> indices = { 0U, 1U, 2U };
    std::vector<GpuSkinningPartition> partitions;
    std::string error;
    EXPECT_FALSE(build_gpu_skinning_partitions(vertices, indices, kMaxGpuSkinningJoints, partitions,
                                               &error));
}

} // namespace
} // namespace isla::client
