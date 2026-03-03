#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include <gtest/gtest.h>

namespace {

bool c_string_is_null_or_empty(const char* value) {
    return value == nullptr || *value == '\0';
}

std::string read_text_file(const std::filesystem::path& file_path) {
    std::ifstream stream(file_path, std::ios::binary);
    if (!stream.is_open()) {
        return "";
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string find_path_in_runfiles_manifest(const std::string& relative_path) {
    const char* manifest_path = std::getenv("RUNFILES_MANIFEST_FILE");
    if (c_string_is_null_or_empty(manifest_path)) {
        return "";
    }

    std::ifstream manifest_stream(manifest_path);
    if (!manifest_stream.is_open()) {
        return "";
    }

    const std::string needle = absl::StrCat("_main/", relative_path);
    std::string line;
    while (std::getline(manifest_stream, line)) {
        const std::vector<absl::string_view> parts = absl::StrSplit(line, absl::MaxSplits(' ', 1));
        if (parts.size() != 2U) {
            continue;
        }

        if (parts.at(0) == needle) {
            return std::string(parts.at(1));
        }
    }

    return "";
}

std::string load_shader_source(std::string_view relative_path_view) {
    const std::string relative_path(relative_path_view);
    std::vector<std::filesystem::path> candidates{
        std::filesystem::path(relative_path_view),
        std::filesystem::path("..") / relative_path_view,
    };

    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (!c_string_is_null_or_empty(test_srcdir)) {
        candidates.emplace_back(std::filesystem::path(test_srcdir) / "_main" / relative_path);
    }

    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    if (!c_string_is_null_or_empty(runfiles_dir)) {
        candidates.emplace_back(std::filesystem::path(runfiles_dir) / "_main" / relative_path);
    }
    const std::string manifest_resolved_path = find_path_in_runfiles_manifest(relative_path);
    if (!manifest_resolved_path.empty()) {
        candidates.emplace_back(manifest_resolved_path);
    }

    for (const std::filesystem::path& candidate : candidates) {
        const std::string text = read_text_file(candidate.lexically_normal());
        if (!text.empty()) {
            return text;
        }
    }

    return "";
}

TEST(ShaderContractTests, VertexShaderDecodesAndTransformsNormalsContract) {
    const std::string vertex_shader = load_shader_source("engine/src/render/shaders/vs_mesh.sc");
    ASSERT_FALSE(vertex_shader.empty()) << "Could not load vertex shader source";

    EXPECT_TRUE(absl::StrContains(vertex_shader, "$input a_normal"));
    EXPECT_TRUE(absl::StrContains(vertex_shader,
                                  "v_world_pos = mul(u_model[0], vec4(a_position, 1.0)).xyz;"));
    EXPECT_TRUE(absl::StrContains(vertex_shader,
                                  "vec3 unpackedNormal = normalize((a_normal.xyz * 2.0) - 1.0);"));
    EXPECT_TRUE(absl::StrContains(
        vertex_shader, "v_normal = normalize(mul(u_model[0], vec4(unpackedNormal, 0.0)).xyz);"));
}

TEST(ShaderContractTests, FragmentShaderUsesDynamicLightingUniformsContract) {
    const std::string fragment_shader = load_shader_source("engine/src/render/shaders/fs_mesh.sc");
    ASSERT_FALSE(fragment_shader.empty()) << "Could not load fragment shader source";

    EXPECT_TRUE(absl::StrContains(fragment_shader, "vec3 normal = -normalize(v_normal);"));
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "vec3 light_dir = normalize(u_dir_light_dir.xyz);"));
    EXPECT_TRUE(absl::StrContains(fragment_shader,
                                  "vec3 view_dir = normalize(u_camera_pos.xyz - v_world_pos);"));
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "vec3 half_dir = normalize(light_dir + view_dir);"));
    EXPECT_TRUE(absl::StrContains(fragment_shader, "vec3 ambient = u_ambient_color.rgb;"));
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "vec3 diffuse = u_dir_light_color.rgb * ndotl;"));
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "vec3 specular = u_dir_light_color.rgb * spec_term;"));
    EXPECT_TRUE(absl::StrContains(fragment_shader, "uniform vec4 u_camera_pos;"));
    EXPECT_TRUE(absl::StrContains(fragment_shader, "uniform vec4 u_spec_params;"));
    EXPECT_TRUE(absl::StrContains(fragment_shader, "(ndotl > 0.0)"))
        << "Specular term should be gated by diffuse-facing check";
    EXPECT_TRUE(absl::StrContains(fragment_shader, "dot(normal, light_dir)"));
}

TEST(ShaderContractTests, ShaderSupportsTextureCoordinatesContract) {
    const std::string vertex_shader = load_shader_source("engine/src/render/shaders/vs_mesh.sc");
    ASSERT_FALSE(vertex_shader.empty()) << "Could not load vertex shader source";
    EXPECT_TRUE(absl::StrContains(vertex_shader, "$input a_position, a_texcoord0"));
    EXPECT_TRUE(
        absl::StrContains(vertex_shader, "$output v_color0, v_normal, v_texcoord0, v_world_pos"));

    const std::string fragment_shader = load_shader_source("engine/src/render/shaders/fs_mesh.sc");
    ASSERT_FALSE(fragment_shader.empty()) << "Could not load fragment shader source";
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "$input v_color0, v_normal, v_texcoord0, v_world_pos"));
    EXPECT_TRUE(absl::StrContains(fragment_shader, "SAMPLER2D(s_texColor, 0);"));
    EXPECT_TRUE(
        absl::StrContains(fragment_shader, "vec4 texColor = texture2D(s_texColor, v_texcoord0);"));
}

TEST(ShaderContractTests, VertexShaderUsesSimulationTimeForPulseContract) {
    const std::string vertex_shader = load_shader_source("engine/src/render/shaders/vs_mesh.sc");
    ASSERT_FALSE(vertex_shader.empty()) << "Could not load vertex shader source";

    EXPECT_TRUE(absl::StrContains(vertex_shader, "float pulse = sin(u_time.x) * 0.1 + 0.9;"));
    EXPECT_FALSE(absl::StrContains(vertex_shader, "float pulse = sin(u_time.y) * 0.1 + 0.9;"));
}

// ---------------------------------------------------------------------------
// Instanced vertex shader contracts
// ---------------------------------------------------------------------------

TEST(ShaderContractTests, InstancedVertexShaderDeclaresInstanceDataInputs) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_instanced.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load instanced vertex shader source";

    EXPECT_TRUE(absl::StrContains(shader, "$input i_data0, i_data1, i_data2, i_data3, i_data4"));
}

TEST(ShaderContractTests, InstancedVertexShaderReadsModelFromInstanceData) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_instanced.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load instanced vertex shader source";

    EXPECT_TRUE(absl::StrContains(shader, "model[0] = i_data0;"));
    EXPECT_TRUE(absl::StrContains(shader, "model[1] = i_data1;"));
    EXPECT_TRUE(absl::StrContains(shader, "model[2] = i_data2;"));
    EXPECT_TRUE(absl::StrContains(shader, "model[3] = i_data3;"));
    EXPECT_TRUE(absl::StrContains(shader, "mul(u_viewProj, worldPos)"));
}

TEST(ShaderContractTests, InstancedVertexShaderProducesSameOutputsAsNonInstanced) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_instanced.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load instanced vertex shader source";

    EXPECT_TRUE(absl::StrContains(shader, "$output v_color0, v_normal, v_texcoord0, v_world_pos"));
    EXPECT_TRUE(absl::StrContains(shader, "v_world_pos = worldPos.xyz;"));
}

TEST(ShaderContractTests, InstancedVertexShaderUsesSimulationTimeForPulseContract) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_instanced.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load instanced vertex shader source";

    EXPECT_TRUE(absl::StrContains(shader, "float pulse = sin(u_time.x) * 0.1 + 0.9;"));
    EXPECT_FALSE(absl::StrContains(shader, "float pulse = sin(u_time.y) * 0.1 + 0.9;"));
}

TEST(ShaderContractTests, InstancedVertexShaderReadsColorFromInstanceData) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_instanced.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load instanced vertex shader source";

    // Phase 2: color comes from per-instance data, not a shared uniform.
    EXPECT_TRUE(absl::StrContains(shader, "i_data4.rgb"));
    EXPECT_TRUE(absl::StrContains(shader, "i_data4.a"));
    EXPECT_FALSE(absl::StrContains(shader, "u_object_color"))
        << "Instanced shader should not reference u_object_color; color is per-instance";
}

TEST(ShaderContractTests, InstancedVaryingDefDeclaresPerInstanceColorAttribute) {
    const std::string varying =
        load_shader_source("engine/src/render/shaders/varying_instanced.def.sc");
    ASSERT_FALSE(varying.empty()) << "Could not load instanced varying definition";

    // Phase 2: i_data4 carries per-instance RGBA color.
    EXPECT_TRUE(absl::StrContains(varying, "i_data4"));
    EXPECT_TRUE(absl::StrContains(varying, "v_world_pos"));
}

TEST(ShaderContractTests, NonInstancedVaryingDefDeclaresWorldPositionVarying) {
    const std::string varying = load_shader_source("engine/src/render/shaders/varying.def.sc");
    ASSERT_FALSE(varying.empty()) << "Could not load varying definition";

    // Phase 1: world position varying is required for view-direction/specular lighting.
    EXPECT_TRUE(absl::StrContains(varying, "v_world_pos"));
}

TEST(ShaderContractTests, SkinnedVertexShaderDeclaresSkinInputsAndPaletteUniform) {
    const std::string shader = load_shader_source("engine/src/render/shaders/vs_mesh_skinned.sc");
    ASSERT_FALSE(shader.empty()) << "Could not load skinned vertex shader source";

    EXPECT_TRUE(absl::StrContains(shader, "$input a_indices, a_weight"));
    EXPECT_TRUE(absl::StrContains(shader, "uniform mat4 u_joint_palette[64];"));
    EXPECT_TRUE(absl::StrContains(shader, "vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));"));
}

} // namespace
