#include "model_intake.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace isla::client::model_intake {
namespace {

class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name_);
        if (existing != nullptr) {
            had_original_ = true;
            original_ = existing;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_original_) {
            set(original_.c_str());
        } else {
#if defined(_WIN32)
            _putenv_s(name_, "");
#else
            unsetenv(name_);
#endif
        }
    }

  private:
    void set(const char* value) {
#if defined(_WIN32)
        _putenv_s(name_, value != nullptr ? value : "");
#else
        if (value == nullptr || value[0] == '\0') {
            unsetenv(name_);
        } else {
            setenv(name_, value, 1);
        }
#endif
    }

    const char* name_ = nullptr;
    bool had_original_ = false;
    std::string original_;
};

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

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> make_minimal_triangle_glb() {
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"max\":[1,1,0],\"min\":[0,0,0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";

    std::vector<std::uint8_t> json_chunk(json.begin(), json.end());
    while ((json_chunk.size() % 4U) != 0U) {
        json_chunk.push_back(static_cast<std::uint8_t>(' '));
    }

    std::vector<std::uint8_t> bin_chunk;
    const float vertices[9] = {
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
    };
    for (float value : vertices) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        bin_chunk.insert(bin_chunk.end(), bytes, bytes + sizeof(float));
    }
    while ((bin_chunk.size() % 4U) != 0U) {
        bin_chunk.push_back(0U);
    }

    const std::uint32_t total_length = 12U + 8U + static_cast<std::uint32_t>(json_chunk.size()) +
                                       8U + static_cast<std::uint32_t>(bin_chunk.size());
    std::vector<std::uint8_t> glb;
    glb.reserve(total_length);

    glb.push_back(static_cast<std::uint8_t>('g'));
    glb.push_back(static_cast<std::uint8_t>('l'));
    glb.push_back(static_cast<std::uint8_t>('T'));
    glb.push_back(static_cast<std::uint8_t>('F'));
    append_u32_le(glb, 2U);
    append_u32_le(glb, total_length);

    append_u32_le(glb, static_cast<std::uint32_t>(json_chunk.size()));
    glb.push_back(static_cast<std::uint8_t>('J'));
    glb.push_back(static_cast<std::uint8_t>('S'));
    glb.push_back(static_cast<std::uint8_t>('O'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.insert(glb.end(), json_chunk.begin(), json_chunk.end());

    append_u32_le(glb, static_cast<std::uint32_t>(bin_chunk.size()));
    glb.push_back(static_cast<std::uint8_t>('B'));
    glb.push_back(static_cast<std::uint8_t>('I'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.push_back(0U);
    glb.insert(glb.end(), bin_chunk.begin(), bin_chunk.end());
    return glb;
}

void write_binary_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

TEST(ModelIntakeTest, ResolvesPreferredNamedDefaultModelFirst) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    write_binary_file(workspace.path() / "models" / "model.glb", glb);
    write_binary_file(workspace.path() / "models" / "aaa.glb", glb);

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models();
    ASSERT_TRUE(result.has_asset);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path).filename().string(), "model.glb");
    EXPECT_EQ(result.source_label, "models_preferred_default");
}

TEST(ModelIntakeTest, ResolvesDeterministicallyByExtensionThenFilenameWhenNoPreferredName) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    write_binary_file(workspace.path() / "models" / "zeta.glb", glb);
    write_binary_file(workspace.path() / "models" / "alpha.glb", glb);
    {
        std::ofstream gltf(workspace.path() / "models" / "beta.gltf");
        ASSERT_TRUE(gltf.is_open());
        gltf << "{}";
    }
    {
        std::ofstream pmx(workspace.path() / "models" / "char.pmx");
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models();
    ASSERT_TRUE(result.has_asset);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path).filename().string(), "alpha.glb");
}

TEST(ModelIntakeTest, AutoConvertsPmxWhenOnlyPmxCandidateExists) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::filesystem::path pmx_path = workspace.path() / "models" / "model.pmx";
    {
        std::ofstream pmx(pmx_path);
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }
    const std::filesystem::path expected_output =
        workspace.path() / "models" / ".isla_converted" / "model.auto.glb";

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    int run_count = 0;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "fake-converter --in {input} --out {output}";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        EXPECT_FALSE(argv.empty());
        ++run_count;
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        write_binary_file(expected_output, glb);
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    EXPECT_TRUE(result.used_pmx_conversion);
    EXPECT_FALSE(result.pmx_conversion_cache_hit);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path), expected_output.lexically_normal());
    EXPECT_EQ(run_count, 1);
}

TEST(ModelIntakeTest, UsesDefaultConverterTemplateWhenNoCommandConfigured) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::filesystem::path pmx_path = workspace.path() / "models" / "model.pmx";
    {
        std::ofstream pmx(pmx_path);
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }
    const std::filesystem::path expected_output =
        workspace.path() / "models" / ".isla_converted" / "model.auto.glb";

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());
    ScopedEnvVar converter_cmd_env("ISLA_PMX_CONVERTER_COMMAND", "");
    ScopedEnvVar converter_ver_env("ISLA_PMX_CONVERTER_VERSION", "");

    int run_count = 0;
    std::string observed_command;
    ResolveStartupAssetOptions options;
    options.run_command = [&](std::span<const std::string> argv) -> int {
        ++run_count;
        observed_command.clear();
        for (const std::string& arg : argv) {
            if (!observed_command.empty()) {
                observed_command += " ";
            }
            observed_command += arg;
        }
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        write_binary_file(expected_output, glb);
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    EXPECT_TRUE(result.used_pmx_conversion);
    EXPECT_EQ(run_count, 1);
    EXPECT_NE(observed_command.find("pmx2gltf"), std::string::npos);
    EXPECT_NE(observed_command.find("--input"), std::string::npos);
    EXPECT_NE(observed_command.find("--output"), std::string::npos);
}

TEST(ModelIntakeTest, UsesPmxConversionCacheWhenSourceAndConverterUnchanged) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::filesystem::path pmx_path = workspace.path() / "models" / "model.pmx";
    {
        std::ofstream pmx(pmx_path);
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }
    const std::filesystem::path expected_output =
        workspace.path() / "models" / ".isla_converted" / "model.auto.glb";

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    int run_count = 0;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "fake-converter --in {input} --out {output}";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        EXPECT_FALSE(argv.empty());
        ++run_count;
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        write_binary_file(expected_output, glb);
        return 0;
    };

    const ResolveStartupAssetResult first = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(first.has_asset);
    ASSERT_TRUE(first.used_pmx_conversion);
    EXPECT_FALSE(first.pmx_conversion_cache_hit);
    EXPECT_EQ(run_count, 1);

    const ResolveStartupAssetResult second = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(second.has_asset);
    ASSERT_TRUE(second.used_pmx_conversion);
    EXPECT_TRUE(second.pmx_conversion_cache_hit);
    EXPECT_EQ(run_count, 1);
}

TEST(ModelIntakeTest, FallsBackToDirectGltfOrGlbWhenPmxConversionFails) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    {
        std::ofstream pmx(workspace.path() / "models" / "model.pmx");
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }
    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    const std::filesystem::path fallback_glb = workspace.path() / "models" / "z_fallback.glb";
    write_binary_file(fallback_glb, glb);

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "fake-converter --in {input} --out {output}";
    options.pmx_converter_version = "v1";
    options.run_command = [](std::span<const std::string> argv) -> int {
        EXPECT_FALSE(argv.empty());
        return 2;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    EXPECT_FALSE(result.used_pmx_conversion);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path), fallback_glb.lexically_normal());
    EXPECT_FALSE(result.warnings.empty());
}

TEST(ModelIntakeTest, ReturnsNoAssetWhenOnlyPmxExistsAndConverterInvocationFails) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    {
        std::ofstream pmx(workspace.path() / "models" / "model.pmx");
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "missing_converter --input {input} --output {output}";
    options.pmx_converter_version = "missing";
    options.run_command = [](std::span<const std::string> argv) -> int {
        EXPECT_FALSE(argv.empty());
        return 127;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    EXPECT_FALSE(result.has_asset);
    EXPECT_FALSE(result.warnings.empty());
    bool found_exit_code = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("exit_code=127") != std::string::npos) {
            found_exit_code = true;
            break;
        }
    }
    EXPECT_TRUE(found_exit_code);
}

TEST(ModelIntakeTest, PassesDangerousFilenameAsSingleArgWithoutShellEvaluation) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::filesystem::path pmx_path = workspace.path() / "models" / "$(id).pmx";
    {
        std::ofstream pmx(pmx_path);
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }
    const std::filesystem::path expected_output =
        workspace.path() / "models" / ".isla_converted" / "$(id).auto.glb";

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    int run_count = 0;
    std::vector<std::string> captured_argv;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "fake-converter --input {input} --output {output}";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        ++run_count;
        captured_argv.assign(argv.begin(), argv.end());
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        write_binary_file(expected_output, glb);
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    ASSERT_EQ(run_count, 1);
    ASSERT_GE(captured_argv.size(), 5U);
    EXPECT_EQ(captured_argv[0], "fake-converter");
    EXPECT_EQ(captured_argv[1], "--input");
    EXPECT_EQ(captured_argv[2], pmx_path.lexically_normal().string());
    EXPECT_EQ(captured_argv[3], "--output");
    EXPECT_EQ(captured_argv[4], expected_output.lexically_normal().string());
}

TEST(ModelIntakeTest, TemplateShellMetacharactersRemainInArgvAndAreNotInterpreted) {
    ScopedTempDir sandbox = ScopedTempDir::create("isla_model_intake_sandbox");
    ScopedTempDir workspace = ScopedTempDir::create("isla_model_intake_workspace");
    ASSERT_TRUE(sandbox.is_valid());
    ASSERT_TRUE(workspace.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace.path() / "models"));

    const std::filesystem::path pmx_path = workspace.path() / "models" / "model.pmx";
    {
        std::ofstream pmx(pmx_path);
        ASSERT_TRUE(pmx.is_open());
        pmx << "pmx";
    }

    ScopedCurrentPath cwd_guard(sandbox.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace.path().string().c_str());

    bool observed_semicolon_arg = false;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template =
        "fake-converter --input {input} --output {output};rm -rf /";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        for (const std::string& arg : argv) {
            if (arg.find(";rm") != std::string::npos) {
                observed_semicolon_arg = true;
            }
        }
        return 127;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    EXPECT_FALSE(result.has_asset);
    EXPECT_TRUE(observed_semicolon_arg);
}

} // namespace
} // namespace isla::client::model_intake
