#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/src/render/include/shader_path_resolver.hpp"

namespace {

constexpr const char* kShaderName = "vs_mesh.bin";

class ScopedTempPathCleanup {
  public:
    explicit ScopedTempPathCleanup(std::filesystem::path path) : path_(std::move(path)) {}

    ~ScopedTempPathCleanup() {
        if (path_.empty()) {
            return;
        }

        std::error_code remove_error;
        const std::uintmax_t removed_count = std::filesystem::remove_all(path_, remove_error);
        if (remove_error) {
            ADD_FAILURE() << "Failed to remove temporary path '" << path_.string()
                          << "': " << remove_error.message();
            return;
        }

        std::error_code exists_error;
        if (removed_count == 0 && std::filesystem::exists(path_, exists_error)) {
            ADD_FAILURE() << "Temporary path cleanup did not remove '" << path_.string() << "'";
        }
    }

    ScopedTempPathCleanup(const ScopedTempPathCleanup&) = delete;
    ScopedTempPathCleanup& operator=(const ScopedTempPathCleanup&) = delete;

  private:
    std::filesystem::path path_;
};

class ScopedCurrentPathGuard {
  public:
    explicit ScopedCurrentPathGuard(const std::filesystem::path& path) {
        std::error_code current_path_error;
        original_path_ = std::filesystem::current_path(current_path_error);
        if (current_path_error) {
            ADD_FAILURE() << "Failed to capture original current working directory: "
                          << current_path_error.message();
            return;
        }

        std::error_code set_path_error;
        std::filesystem::current_path(path, set_path_error);
        if (set_path_error) {
            ADD_FAILURE() << "Failed to set current working directory to '" << path.string()
                          << "': " << set_path_error.message();
            return;
        }

        armed_ = true;
    }

    ~ScopedCurrentPathGuard() {
        if (!armed_) {
            return;
        }

        std::error_code restore_error;
        std::filesystem::current_path(original_path_, restore_error);
        if (restore_error) {
            ADD_FAILURE() << "Failed to restore original current working directory '"
                          << original_path_.string() << "': " << restore_error.message();
        }
    }

    ScopedCurrentPathGuard(const ScopedCurrentPathGuard&) = delete;
    ScopedCurrentPathGuard& operator=(const ScopedCurrentPathGuard&) = delete;

  private:
    std::filesystem::path original_path_;
    bool armed_ = false;
};

std::string normalize_slashes(std::string value) {
    std::ranges::replace(value, '\\', '/');
    return value;
}

bool contains_path(const std::vector<std::string>& candidates, const std::string& expected_raw) {
    const std::string expected = normalize_slashes(expected_raw);
    return std::ranges::any_of(candidates, [&](const std::string& candidate) {
        return normalize_slashes(candidate) == expected;
    });
}

// --- Standalone tests (no manifest file needed) ---

TEST(ShaderPathResolverTests, AlwaysIncludesWorkspaceAndBazelBinCandidatesFirst) {
    const std::vector<std::string> candidates = isla::client::build_shader_path_candidates({
        .shader_file_name = kShaderName,
        .executable_base_path = "",
        .runfiles_dir = "",
        .test_srcdir = "",
        .runfiles_manifest_file = "",
    });

    ASSERT_GE(candidates.size(), 4U);
    EXPECT_EQ("engine/src/render/shaders/dx11/vs_mesh.bin", normalize_slashes(candidates.at(0)));
    EXPECT_EQ("bazel-bin/engine/src/render/shaders/dx11/vs_mesh.bin",
              normalize_slashes(candidates.at(1)));
}

TEST(ShaderPathResolverTests, IncludesRunfilesAndExecutableRelativeCandidatesWhenProvided) {
    const std::vector<std::string> candidates = isla::client::build_shader_path_candidates({
        .shader_file_name = kShaderName,
        .executable_base_path = "C:/repo/bazel-bin/client/src/",
        .runfiles_dir = "C:/repo/runfiles",
        .test_srcdir = "C:/repo/testsrc",
        .runfiles_manifest_file = "",
    });

    EXPECT_TRUE(contains_path(candidates,
                              "C:/repo/runfiles/_main/engine/src/render/shaders/dx11/vs_mesh.bin"));
    EXPECT_TRUE(contains_path(candidates,
                              "C:/repo/testsrc/_main/engine/src/render/shaders/dx11/vs_mesh.bin"));
}

TEST(ShaderPathResolverTests, BazelBinLaunchKeepsWorkspacePathsFirst) {
    const std::vector<std::string> candidates = isla::client::build_shader_path_candidates({
        .shader_file_name = kShaderName,
        .executable_base_path = "C:/repo/bazel-bin/client/src/",
        .runfiles_dir = "",
        .test_srcdir = "",
        .runfiles_manifest_file = "",
    });

    ASSERT_GE(candidates.size(), 6U);
    EXPECT_EQ("engine/src/render/shaders/dx11/vs_mesh.bin", normalize_slashes(candidates.at(0)));
    EXPECT_EQ("bazel-bin/engine/src/render/shaders/dx11/vs_mesh.bin",
              normalize_slashes(candidates.at(1)));
    EXPECT_EQ("client/src/shaders/dx11/vs_mesh.bin", normalize_slashes(candidates.at(2)));
    EXPECT_EQ("bazel-bin/client/src/shaders/dx11/vs_mesh.bin", normalize_slashes(candidates.at(3)));
}

// --- Fixture for tests that create a temporary manifest file ---

class ShaderPathResolverManifestTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        manifest_path_ =
            temp_dir /
            ("isla_shader_path_resolver_test_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".runfiles_manifest");
    }

    void TearDown() override {
        if (manifest_path_.empty()) {
            return;
        }

        std::error_code remove_error;
        const std::uintmax_t removed_count =
            std::filesystem::remove_all(manifest_path_, remove_error);
        if (remove_error) {
            ADD_FAILURE() << "Failed to remove manifest '" << manifest_path_.string()
                          << "': " << remove_error.message();
            return;
        }

        std::error_code exists_error;
        if (removed_count == 0 && std::filesystem::exists(manifest_path_, exists_error)) {
            ADD_FAILURE() << "Manifest cleanup did not remove '" << manifest_path_.string() << "'";
        }
    }

    void write_manifest(const std::string& content) {
        std::ofstream manifest_stream(manifest_path_);
        ASSERT_TRUE(manifest_stream.is_open()) << "Failed to open " << manifest_path_.string();
        manifest_stream << content;
    }

    [[nodiscard]] std::vector<std::string> build_candidates_with_manifest() const {
        return isla::client::build_shader_path_candidates({
            .shader_file_name = kShaderName,
            .executable_base_path = "",
            .runfiles_dir = "",
            .test_srcdir = "",
            .runfiles_manifest_file = manifest_path_.string(),
        });
    }

    std::filesystem::path manifest_path_;
};

TEST_F(ShaderPathResolverManifestTest, IncludesRunfilesManifestCandidateWhenProvided) {
    write_manifest("_main/engine/src/render/shaders/dx11/vs_mesh.bin C:/repo/out/vs_mesh.bin\n");

    const std::vector<std::string> candidates = build_candidates_with_manifest();
    EXPECT_TRUE(contains_path(candidates, "C:/repo/out/vs_mesh.bin"));
}

TEST_F(ShaderPathResolverManifestTest, IncludesRunfilesManifestCandidateWhenPathContainsSpaces) {
    write_manifest("_main/engine/src/render/shaders/dx11/vs_mesh.bin "
                   "C:/Program Files/isla/vs_mesh.bin\n");

    const std::vector<std::string> candidates = build_candidates_with_manifest();
    EXPECT_TRUE(contains_path(candidates, "C:/Program Files/isla/vs_mesh.bin"));
}

TEST_F(ShaderPathResolverManifestTest, IgnoresManifestEntriesWithLeadingWhitespace) {
    write_manifest(" _main/engine/src/render/shaders/dx11/vs_mesh.bin "
                   "C:/repo/incorrect/vs_mesh.bin\n"
                   "_main/engine/src/render/shaders/dx11/vs_mesh.bin C:/repo/out/vs_mesh.bin\n");

    const std::vector<std::string> candidates = build_candidates_with_manifest();
    EXPECT_TRUE(contains_path(candidates, "C:/repo/out/vs_mesh.bin"));
    EXPECT_FALSE(contains_path(candidates, "C:/repo/incorrect/vs_mesh.bin"));
}

// --- Tests with custom temp directory structure (not using the manifest fixture) ---

TEST(ShaderPathResolverTests, DiscoversRunfilesManifestBesideExecutableBasePath) {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() /
        ("isla_shader_path_resolver_manifest_dir_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScopedTempPathCleanup temp_dir_cleanup(temp_dir);
    std::error_code create_error;
    ASSERT_TRUE(std::filesystem::create_directories(temp_dir, create_error)) << create_error;

    const std::filesystem::path manifest_path = temp_dir / "isla_client.exe.runfiles_manifest";
    ScopedTempPathCleanup manifest_cleanup(manifest_path);
    {
        std::ofstream manifest_stream(manifest_path);
        ASSERT_TRUE(manifest_stream.is_open());
        manifest_stream
            << "_main/engine/src/render/shaders/dx11/vs_mesh.bin C:/repo/out/vs_mesh.bin\n";
    }

    const std::vector<std::string> candidates = isla::client::build_shader_path_candidates({
        .shader_file_name = kShaderName,
        .executable_base_path = temp_dir.string(),
        .runfiles_dir = "",
        .test_srcdir = "",
        .runfiles_manifest_file = "",
    });

    EXPECT_TRUE(contains_path(candidates, "C:/repo/out/vs_mesh.bin"));
}

TEST(ShaderPathResolverTests, DiscoversManifestInCurrentWorkingDirectory) {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() /
        ("isla_shader_path_resolver_cwd_manifest_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScopedTempPathCleanup temp_dir_cleanup(temp_dir);

    std::error_code create_error;
    ASSERT_TRUE(std::filesystem::create_directories(temp_dir, create_error)) << create_error;

    const std::filesystem::path manifest_path = temp_dir / "MANIFEST";
    {
        std::ofstream manifest_stream(manifest_path);
        ASSERT_TRUE(manifest_stream.is_open());
        manifest_stream
            << "_main/engine/src/render/shaders/dx11/vs_mesh.bin C:/repo/out/vs_mesh.bin\n";
    }

    ScopedCurrentPathGuard current_path_guard(temp_dir);

    const std::vector<std::string> candidates = isla::client::build_shader_path_candidates({
        .shader_file_name = kShaderName,
        .executable_base_path = "",
        .runfiles_dir = "",
        .test_srcdir = "",
        .runfiles_manifest_file = "",
    });

    EXPECT_TRUE(contains_path(candidates, "C:/repo/out/vs_mesh.bin"));
}

} // namespace
