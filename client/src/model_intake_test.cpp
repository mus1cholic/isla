#include "model_intake.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
    const std::array<float, 9U> vertices = {
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
    };
    for (float value : vertices) {
        std::uint8_t bytes[sizeof(value)]{};
        std::memcpy(bytes, &value, sizeof(value));
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

::testing::AssertionResult write_binary_file(const std::filesystem::path& path,
                                             std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return ::testing::AssertionFailure() << "failed to open binary file: " << path;
    }
    stream.write(static_cast<const char*>(static_cast<const void*>(bytes.data())),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream.good()) {
        return ::testing::AssertionFailure() << "failed to write binary file: " << path;
    }
    return ::testing::AssertionSuccess();
}

class ModelIntakeFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        sandbox_ = ScopedTempDir::create("isla_model_intake_sandbox");
        workspace_ = ScopedTempDir::create("isla_model_intake_workspace");
        ASSERT_TRUE(sandbox_.is_valid());
        ASSERT_TRUE(workspace_.is_valid());
        ASSERT_TRUE(std::filesystem::create_directories(models_dir()));

        cwd_guard_ = std::make_unique<ScopedCurrentPath>(sandbox_.path());
        ASSERT_TRUE(cwd_guard_->is_armed());
        workspace_env_ = std::make_unique<ScopedEnvVar>("BUILD_WORKSPACE_DIRECTORY",
                                                        workspace_.path().string().c_str());
    }

    [[nodiscard]] std::filesystem::path models_dir() const {
        return workspace_.path() / "models";
    }

    [[nodiscard]] std::filesystem::path model_path(std::string_view filename) const {
        return models_dir() / std::string(filename);
    }

    [[nodiscard]] std::filesystem::path converted_output_for(std::string_view stem) const {
        return models_dir() / ".isla_converted" / (std::string(stem) + ".auto.glb");
    }

    ::testing::AssertionResult write_glb(std::string_view filename) const {
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        return write_binary_file(model_path(filename), glb);
    }

    ::testing::AssertionResult write_text(std::string_view filename,
                                          std::string_view contents) const {
        std::ofstream out(model_path(filename));
        if (!out.is_open()) {
            return ::testing::AssertionFailure()
                   << "failed to open text file: " << model_path(filename);
        }
        out << contents;
        if (!out.good()) {
            return ::testing::AssertionFailure()
                   << "failed to write text file: " << model_path(filename);
        }
        return ::testing::AssertionSuccess();
    }

    ScopedTempDir sandbox_;
    ScopedTempDir workspace_;
    std::unique_ptr<ScopedCurrentPath> cwd_guard_;
    std::unique_ptr<ScopedEnvVar> workspace_env_;
};

TEST_F(ModelIntakeFixture, ResolvesPreferredNamedDefaultModelFirst) {
    ASSERT_TRUE(write_glb("model.glb"));
    ASSERT_TRUE(write_glb("aaa.glb"));

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models();
    ASSERT_TRUE(result.has_asset);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path).filename().string(), "model.glb");
    EXPECT_EQ(result.source_label, "models_preferred_default");
}

TEST_F(ModelIntakeFixture, ResolvesDeterministicallyByExtensionThenFilenameWhenNoPreferredName) {
    ASSERT_TRUE(write_glb("zeta.glb"));
    ASSERT_TRUE(write_glb("alpha.glb"));
    ASSERT_TRUE(write_text("beta.gltf", "{}"));
    ASSERT_TRUE(write_text("char.pmx", "pmx"));

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models();
    ASSERT_TRUE(result.has_asset);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path).filename().string(), "alpha.glb");
}

TEST_F(ModelIntakeFixture, AutoConvertsPmxWhenOnlyPmxCandidateExists) {
    const std::filesystem::path pmx_path = model_path("model.pmx");
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("model");

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
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    EXPECT_TRUE(result.used_pmx_conversion);
    EXPECT_FALSE(result.pmx_conversion_cache_hit);
    EXPECT_EQ(std::filesystem::path(result.runtime_asset_path), expected_output.lexically_normal());
    EXPECT_EQ(run_count, 1);
}

TEST_F(ModelIntakeFixture, UsesDefaultConverterTemplateWhenNoCommandConfigured) {
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("model");
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
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
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

TEST_F(ModelIntakeFixture, UsesPmxConversionCacheWhenSourceAndConverterUnchanged) {
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("model");

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
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
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

TEST_F(ModelIntakeFixture, FallsBackToDirectGltfOrGlbWhenPmxConversionFails) {
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path fallback_glb = model_path("z_fallback.glb");
    ASSERT_TRUE(write_glb("z_fallback.glb"));

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

TEST_F(ModelIntakeFixture, ReturnsNoAssetWhenOnlyPmxExistsAndConverterInvocationFails) {
    ASSERT_TRUE(write_text("model.pmx", "pmx"));

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

TEST_F(ModelIntakeFixture, PassesDangerousFilenameAsSingleArgWithoutShellEvaluation) {
    const std::filesystem::path pmx_path = model_path("$(id).pmx");
    ASSERT_TRUE(write_text("$(id).pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("$(id)");

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
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
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

TEST_F(ModelIntakeFixture, TemplateShellMetacharactersRemainInArgvAndAreNotInterpreted) {
    ASSERT_TRUE(write_text("model.pmx", "pmx"));

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

TEST_F(ModelIntakeFixture, PreservesWindowsStyleExecutablePathBackslashesInTemplate) {
    const std::filesystem::path pmx_path = model_path("model.pmx");
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("model");

    std::vector<std::string> captured_argv;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template =
        "C:\\Tools\\pmx2gltf.exe --input {input} --output {output}";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        captured_argv.assign(argv.begin(), argv.end());
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    ASSERT_GE(captured_argv.size(), 5U);
    EXPECT_EQ(captured_argv[0], "C:\\Tools\\pmx2gltf.exe");
    EXPECT_EQ(captured_argv[1], "--input");
    EXPECT_EQ(captured_argv[2], pmx_path.lexically_normal().string());
    EXPECT_EQ(captured_argv[3], "--output");
    EXPECT_EQ(captured_argv[4], expected_output.lexically_normal().string());
}

TEST_F(ModelIntakeFixture, AppendsMissingOutputArgWhenOnlyInputTokenProvided) {
    const std::filesystem::path pmx_path = model_path("model.pmx");
    ASSERT_TRUE(write_text("model.pmx", "pmx"));
    const std::filesystem::path expected_output = converted_output_for("model");

    std::vector<std::string> captured_argv;
    ResolveStartupAssetOptions options;
    options.pmx_converter_command_template = "fake-converter --input {input}";
    options.pmx_converter_version = "v1";
    options.run_command = [&](std::span<const std::string> argv) -> int {
        captured_argv.assign(argv.begin(), argv.end());
        std::error_code ec;
        std::filesystem::create_directories(expected_output.parent_path(), ec);
        if (ec) {
            return 1;
        }
        const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
        const ::testing::AssertionResult write_result = write_binary_file(expected_output, glb);
        if (!write_result) {
            ADD_FAILURE() << write_result.message();
            return 1;
        }
        return 0;
    };

    const ResolveStartupAssetResult result = resolve_startup_asset_from_models(options);
    ASSERT_TRUE(result.has_asset);
    ASSERT_FALSE(result.warnings.empty());
    bool found_template_warning = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("{output}") != std::string::npos) {
            found_template_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_template_warning);
    ASSERT_GE(captured_argv.size(), 4U);
    EXPECT_EQ(captured_argv[0], "fake-converter");
    EXPECT_EQ(captured_argv[1], "--input");
    EXPECT_EQ(captured_argv[2], pmx_path.lexically_normal().string());
    EXPECT_EQ(captured_argv.back(), expected_output.lexically_normal().string());
}

} // namespace
} // namespace isla::client::model_intake
