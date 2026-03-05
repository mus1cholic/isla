#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "engine/src/render/include/shader_path_resolver.hpp"

namespace {

using isla::client::find_existing_shader_path;
using isla::client::ShaderPathLookup;

#ifndef _WIN32
constexpr std::string_view kLinuxDx11StubPrefix = "isla-dx11-shader-stub";
#endif

void expect_shader_binary_is_resolvable(const char* shader_file_name) {
    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* runfiles_manifest_file = std::getenv("RUNFILES_MANIFEST_FILE");
    std::ostringstream diag;
    diag << "RUNFILES_DIR=" << (runfiles_dir == nullptr ? "<null>" : runfiles_dir) << ", "
         << "TEST_SRCDIR=" << (test_srcdir == nullptr ? "<null>" : test_srcdir) << ", "
         << "RUNFILES_MANIFEST_FILE="
         << (runfiles_manifest_file == nullptr ? "<null>" : runfiles_manifest_file);

    const std::string resolved_path = find_existing_shader_path(ShaderPathLookup{
        .shader_file_name = shader_file_name,
        .executable_base_path = "",
        .runfiles_dir = runfiles_dir == nullptr ? "" : runfiles_dir,
        .test_srcdir = test_srcdir == nullptr ? "" : test_srcdir,
        .runfiles_manifest_file = runfiles_manifest_file == nullptr ? "" : runfiles_manifest_file,
    });

    ASSERT_FALSE(resolved_path.empty())
        << "Could not resolve shader binary: " << shader_file_name << " (" << diag.str() << ")";
    const std::filesystem::path shader_path(resolved_path);
    ASSERT_TRUE(std::filesystem::exists(shader_path)) << resolved_path;
    ASSERT_GT(std::filesystem::file_size(shader_path), 0U) << resolved_path;
#ifndef _WIN32
    std::ifstream shader_stream(shader_path, std::ios::binary);
    ASSERT_TRUE(shader_stream.is_open()) << resolved_path;
    std::string prefix(kLinuxDx11StubPrefix.size(), '\0');
    shader_stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    EXPECT_EQ(prefix, std::string(kLinuxDx11StubPrefix))
        << "Expected Linux placeholder shader stub prefix in " << resolved_path;
#endif
}

TEST(ShaderBinaryRuntimeTests, VertexShaderBinaryIsResolvableAtRuntime) {
    expect_shader_binary_is_resolvable("vs_mesh.bin");
}

TEST(ShaderBinaryRuntimeTests, FragmentShaderBinaryIsResolvableAtRuntime) {
    expect_shader_binary_is_resolvable("fs_mesh.bin");
}

TEST(ShaderBinaryRuntimeTests, InstancedVertexShaderBinaryIsResolvableAtRuntime) {
    expect_shader_binary_is_resolvable("vs_mesh_instanced.bin");
}

TEST(ShaderBinaryRuntimeTests, SkinnedVertexShaderBinaryIsResolvableAtRuntime) {
    expect_shader_binary_is_resolvable("vs_mesh_skinned.bin");
}

} // namespace
