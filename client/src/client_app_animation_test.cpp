#include "client_app.hpp"

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "client_app_test_hooks.hpp"

namespace isla::client {
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
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

  private:
    std::filesystem::path original_;
};

std::filesystem::path make_unique_temp_dir() {
    const auto base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 100; ++i) {
        const auto candidate = base / ("isla_client_app_test_" + std::to_string(i) + "_" +
                                       std::to_string(std::rand()));
        std::error_code ec;
        if (std::filesystem::create_directories(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
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
    bin_chunk.reserve(36U);
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

    auto append_u32_le = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    };

    const std::uint32_t json_chunk_length = static_cast<std::uint32_t>(json_chunk.size());
    const std::uint32_t bin_chunk_length = static_cast<std::uint32_t>(bin_chunk.size());
    const std::uint32_t total_length = 12U + 8U + json_chunk_length + 8U + bin_chunk_length;

    std::vector<std::uint8_t> glb;
    glb.reserve(total_length);

    // GLB header.
    glb.push_back(static_cast<std::uint8_t>('g'));
    glb.push_back(static_cast<std::uint8_t>('l'));
    glb.push_back(static_cast<std::uint8_t>('T'));
    glb.push_back(static_cast<std::uint8_t>('F'));
    append_u32_le(glb, 2U);
    append_u32_le(glb, total_length);

    // JSON chunk.
    append_u32_le(glb, json_chunk_length);
    glb.push_back(static_cast<std::uint8_t>('J'));
    glb.push_back(static_cast<std::uint8_t>('S'));
    glb.push_back(static_cast<std::uint8_t>('O'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.insert(glb.end(), json_chunk.begin(), json_chunk.end());

    // BIN chunk.
    append_u32_le(glb, bin_chunk_length);
    glb.push_back(static_cast<std::uint8_t>('B'));
    glb.push_back(static_cast<std::uint8_t>('I'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.push_back(0U);
    glb.insert(glb.end(), bin_chunk.begin(), bin_chunk.end());

    return glb;
}

class FakeSdlRuntime final : public ISdlRuntime {
  public:
    std::uint64_t now_ticks_ns = 0U;
    int pixel_width = 1280;
    int pixel_height = 720;
    mutable std::vector<SDL_Event> queued_events;

    [[nodiscard]] std::uint64_t get_ticks_ns() const override {
        return now_ticks_ns;
    }
    [[nodiscard]] bool init_video() const override {
        return true;
    }
    void quit() const override {}
    [[nodiscard]] bool has_primary_display() const override {
        return true;
    }
    [[nodiscard]] SDL_Window* create_window(const char* title, int width, int height,
                                            std::uint64_t flags) const override {
        (void)title;
        (void)width;
        (void)height;
        (void)flags;
        return reinterpret_cast<SDL_Window*>(0x1);
    }
    [[nodiscard]] SDL_Renderer* create_renderer(SDL_Window* window) const override {
        (void)window;
        return nullptr;
    }
    void destroy_renderer(SDL_Renderer* renderer) const override {
        (void)renderer;
    }
    void destroy_window(SDL_Window* window) const override {
        (void)window;
    }
    void maximize_window(SDL_Window* window) const override {
        (void)window;
    }
    [[nodiscard]] bool poll_event(SDL_Event* event) const override {
        if (queued_events.empty()) {
            return false;
        }
        *event = queued_events.front();
        queued_events.erase(queued_events.begin());
        return true;
    }
    [[nodiscard]] const bool* get_keyboard_state(int* key_count) const override {
        static const std::array<bool, SDL_SCANCODE_COUNT> keys{};
        if (key_count != nullptr) {
            *key_count = SDL_SCANCODE_COUNT;
        }
        return keys.data();
    }
    [[nodiscard]] bool get_window_size_in_pixels(SDL_Window* window, int* width,
                                                 int* height) const override {
        (void)window;
        if (width != nullptr) {
            *width = pixel_width;
        }
        if (height != nullptr) {
            *height = pixel_height;
        }
        return true;
    }
    [[nodiscard]] bool get_window_size(SDL_Window* window, int* width, int* height) const override {
        return get_window_size_in_pixels(window, width, height);
    }
    void set_window_bordered(SDL_Window* window, bool bordered) const override {
        (void)window;
        (void)bordered;
    }
    [[nodiscard]] bool set_window_relative_mouse_mode(SDL_Window* window,
                                                      bool enabled) const override {
        (void)window;
        (void)enabled;
        return true;
    }
};

animated_gltf::AnimatedGltfAsset make_test_asset_with_two_clips() {
    animated_gltf::AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(1U);
    asset.bind_local_transforms.resize(1U);
    asset.bind_prefix_matrices = { Mat4::identity() };

    animated_gltf::SkinnedPrimitive primitive;
    primitive.vertices = {
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 1.0F, .y = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 1.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };
    asset.primitives.push_back(std::move(primitive));

    animated_gltf::AnimationClip idle;
    idle.name = "idle";
    idle.duration_seconds = 1.0F;
    idle.joint_tracks.resize(1U);
    idle.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };

    animated_gltf::AnimationClip walk;
    walk.name = "walk";
    walk.duration_seconds = 1.0F;
    walk.joint_tracks.resize(1U);
    walk.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 10.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 12.0F, .y = 0.0F, .z = 0.0F } },
    };

    asset.clips.push_back(std::move(idle));
    asset.clips.push_back(std::move(walk));
    return asset;
}

animated_gltf::AnimatedGltfAsset make_large_joint_test_asset() {
    animated_gltf::AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(66U);
    asset.bind_local_transforms.resize(66U);
    asset.bind_prefix_matrices.assign(66U, Mat4::identity());

    animated_gltf::SkinnedPrimitive primitive;
    primitive.vertices.reserve(66U);
    primitive.indices.reserve(66U);
    for (std::uint16_t joint = 0U; joint < 66U; ++joint) {
        primitive.vertices.push_back(animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = static_cast<float>(joint), .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { joint, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        });
        primitive.indices.push_back(static_cast<std::uint32_t>(joint));
    }
    asset.primitives.push_back(std::move(primitive));

    animated_gltf::AnimationClip idle;
    idle.name = "idle";
    idle.duration_seconds = 1.0F;
    idle.joint_tracks.resize(66U);
    idle.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };
    animated_gltf::AnimationClip walk = idle;
    walk.name = "walk";
    animated_gltf::AnimationClip action = idle;
    action.name = "action";
    asset.clips.push_back(std::move(idle));
    asset.clips.push_back(std::move(walk));
    asset.clips.push_back(std::move(action));
    return asset;
}

TEST(ClientAppAnimationTest, NonMonotonicClockClampsTickDeltaToZero) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 100U);
    runtime.now_ticks_ns = 50U;

    internal::ClientAppTestHooks::tick(app);
    EXPECT_NEAR(internal::ClientAppTestHooks::world(app).sim_time_seconds(), 0.0F, 1.0e-6F);
}

TEST(ClientAppAnimationTest, TickAdvancesAnimationAndMeshTriangles) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 0U);
    runtime.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app);
    ASSERT_FALSE(world.meshes().empty());
    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_NEAR(world.sim_time_seconds(), 0.5F, 1.0e-4F);
    EXPECT_NEAR(world.meshes()[0].triangles()[0].a.x, 1.0F, 1.0e-4F);
}

TEST(ClientAppAnimationTest, AnimatedTickUpdatesMeshInPlaceWithoutTriangleReallocation) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 0U);
    runtime.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app);
    ASSERT_FALSE(world.meshes().empty());
    const Triangle* triangles_ptr_after_first_tick = world.meshes()[0].triangles().data();
    const std::size_t triangles_capacity_after_first_tick =
        world.meshes()[0].triangles().capacity();

    runtime.now_ticks_ns = 1000000000ULL;
    internal::ClientAppTestHooks::tick(app);

    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_EQ(world.meshes()[0].triangles().data(), triangles_ptr_after_first_tick);
    EXPECT_EQ(world.meshes()[0].triangles().capacity(), triangles_capacity_after_first_tick);
}

TEST(ClientAppAnimationTest, AnimatedTickKeepsTriangleStorageStableAcrossManyFrames) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 0U);
    runtime.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app);
    ASSERT_FALSE(world.meshes().empty());
    const Triangle* stable_ptr = world.meshes()[0].triangles().data();
    const std::size_t stable_capacity = world.meshes()[0].triangles().capacity();

    for (int frame = 0; frame < 120; ++frame) {
        runtime.now_ticks_ns += 16666666ULL;
        internal::ClientAppTestHooks::tick(app);
        ASSERT_FALSE(world.meshes()[0].triangles().empty());
        EXPECT_EQ(world.meshes()[0].triangles().data(), stable_ptr);
        EXPECT_EQ(world.meshes()[0].triangles().capacity(), stable_capacity);
    }
}

TEST(ClientAppAnimationTest, AnimatedTickDefersBoundsRecomputeUntilIntervalBoundary) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 0U);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app);
    ASSERT_FALSE(world.meshes().empty());
    const BoundingSphere initial_bounds = world.meshes()[0].local_bounds();

    for (int frame = 0; frame < 29; ++frame) {
        runtime.now_ticks_ns += 16666666ULL;
        internal::ClientAppTestHooks::tick(app);
    }

    const BoundingSphere deferred_bounds = world.meshes()[0].local_bounds();
    EXPECT_FLOAT_EQ(deferred_bounds.center.x, initial_bounds.center.x);
    EXPECT_FLOAT_EQ(deferred_bounds.center.y, initial_bounds.center.y);
    EXPECT_FLOAT_EQ(deferred_bounds.center.z, initial_bounds.center.z);
    EXPECT_FLOAT_EQ(deferred_bounds.radius, initial_bounds.radius);

    runtime.now_ticks_ns += 16666666ULL;
    internal::ClientAppTestHooks::tick(app);
    const BoundingSphere refreshed_bounds = world.meshes()[0].local_bounds();
    EXPECT_GT(refreshed_bounds.center.x, initial_bounds.center.x + 0.1F);
}

TEST(ClientAppAnimationTest, GpuAuthoritativeAnimationUpdatesPaletteWithoutGeometryChurn) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app, true);
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    internal::ClientAppTestHooks::set_last_tick_ns(app, 0U);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app);
    ASSERT_FALSE(world.meshes().empty());
    ASSERT_TRUE(world.meshes()[0].has_skinned_geometry());
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const std::uint64_t stable_geometry_revision = world.meshes()[0].geometry_revision();

    runtime.now_ticks_ns = 500000000ULL;
    internal::ClientAppTestHooks::tick(app);
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const float tx_after_first_tick = world.meshes()[0].skin_palette()[0].elements[12];
    EXPECT_NEAR(tx_after_first_tick, 1.0F, 1.0e-4F);
    EXPECT_EQ(world.meshes()[0].geometry_revision(), stable_geometry_revision);

    runtime.now_ticks_ns = 1000000000ULL;
    internal::ClientAppTestHooks::tick(app);
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const float tx_after_second_tick = world.meshes()[0].skin_palette()[0].elements[12];
    EXPECT_NEAR(tx_after_second_tick, 0.0F, 1.0e-4F);
    EXPECT_EQ(world.meshes()[0].geometry_revision(), stable_geometry_revision);
}

TEST(ClientAppAnimationTest, GpuAuthoritativeLargeSkeletonIsPartitionedToLocalPaletteBudget) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_large_joint_test_asset());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app, true);
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app);
    ASSERT_GE(world.meshes().size(), 2U);
    for (const MeshData& mesh : world.meshes()) {
        ASSERT_TRUE(mesh.has_skinned_geometry());
        ASSERT_LE(mesh.skin_palette().size(), 64U);
        for (const SkinnedMeshVertex& vertex : mesh.skinned_vertices()) {
            EXPECT_LT(vertex.joints[0], 64U);
        }
    }
}

TEST(ClientAppAnimationTest, GpuAuthoritativePartitioningIsStableAcrossRepeatedPopulate) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_large_joint_test_asset());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app, true);

    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    const RenderWorld& first_world = internal::ClientAppTestHooks::world(app);
    ASSERT_FALSE(first_world.meshes().empty());
    std::vector<std::size_t> first_palette_sizes;
    first_palette_sizes.reserve(first_world.meshes().size());
    for (const MeshData& mesh : first_world.meshes()) {
        first_palette_sizes.push_back(mesh.skin_palette().size());
    }
    const std::size_t first_mesh_count = first_world.meshes().size();
    const std::size_t first_object_count = first_world.objects().size();

    internal::ClientAppTestHooks::populate_world_from_animated_asset(app);
    const RenderWorld& second_world = internal::ClientAppTestHooks::world(app);
    ASSERT_EQ(second_world.meshes().size(), first_mesh_count);
    ASSERT_EQ(second_world.objects().size(), first_object_count);
    ASSERT_EQ(second_world.meshes().size(), first_palette_sizes.size());
    for (std::size_t i = 0U; i < second_world.meshes().size(); ++i) {
        EXPECT_EQ(second_world.meshes()[i].skin_palette().size(), first_palette_sizes[i]);
    }
}

TEST(ClientAppAnimationTest, LoadStartupMeshResetsGpuAuthoritativeFlagWhenRendererUnsupported) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app, true);

    ScopedEnvVar anim_env("ISLA_ANIMATED_GLTF_ASSET", "");
    ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");
    internal::ClientAppTestHooks::load_startup_mesh(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::gpu_skinning_authoritative(app));
}

TEST(ClientAppAnimationTest, EnvironmentConfigSelectsClipAndClampMode) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::set_animated_asset(app, make_test_asset_with_two_clips());

    ScopedEnvVar clip_env("ISLA_ANIM_CLIP", "walk");
    ScopedEnvVar mode_env("ISLA_ANIM_PLAYBACK_MODE", "clamp");

    internal::ClientAppTestHooks::configure_animation_playback_from_environment(app);

    const auto& state = internal::ClientAppTestHooks::animation_playback(app).state();
    EXPECT_EQ(state.clip_index, 1U);
    EXPECT_EQ(state.playback_mode, animated_gltf::ClipPlaybackMode::Clamp);
}

TEST(ClientAppAnimationTest, FallbackWhenAnimatedAssetFailsToLoad) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    ScopedEnvVar anim_env("ISLA_ANIMATED_GLTF_ASSET", "missing_file.gltf");
    ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");

    internal::ClientAppTestHooks::load_startup_mesh(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::has_animated_asset(app));
    EXPECT_TRUE(internal::ClientAppTestHooks::world(app).meshes().empty());
}

TEST(ClientAppAnimationTest, LoadStartupMeshUsesWorkspaceDefaultModelPathWhenUnset) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    const std::filesystem::path sandbox_dir = make_unique_temp_dir();
    const std::filesystem::path workspace_dir = make_unique_temp_dir();
    ASSERT_FALSE(sandbox_dir.empty());
    ASSERT_FALSE(workspace_dir.empty());
    ASSERT_TRUE(std::filesystem::create_directories(workspace_dir / "models"));

    const std::filesystem::path default_glb_path = workspace_dir / "models" / "model.glb";
    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    {
        std::ofstream out(default_glb_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(glb.data()),
                  static_cast<std::streamsize>(glb.size()));
    }

    ScopedCurrentPath cwd_guard(sandbox_dir);
    ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
    ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.string().c_str());

    internal::ClientAppTestHooks::load_startup_mesh(app);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app);
    ASSERT_EQ(world.meshes().size(), 1U);
    ASSERT_EQ(world.objects().size(), 1U);
    EXPECT_FALSE(world.meshes()[0].triangles().empty());
}

TEST(ClientAppAnimationTest, StaticFallbackAppliesVisibleAutoFitTransform) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    const std::filesystem::path temp_dir = make_unique_temp_dir();
    ASSERT_FALSE(temp_dir.empty());
    const std::filesystem::path obj_path = temp_dir / "triangle.obj";
    {
        std::ofstream out(obj_path);
        ASSERT_TRUE(out.is_open());
        out << "v 0 0 0\n";
        out << "v 1 0 0\n";
        out << "v 0 1 0\n";
        out << "f 1 2 3\n";
    }

    ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
    ScopedEnvVar mesh_env("ISLA_MESH_ASSET", obj_path.string().c_str());

    internal::ClientAppTestHooks::load_startup_mesh(app);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app);
    ASSERT_EQ(world.objects().size(), 1U);
    const Transform& transform = world.objects()[0].transform;
    EXPECT_GT(transform.scale.x, 1.0F);
    EXPECT_GT(transform.scale.y, 1.0F);
    EXPECT_GT(transform.scale.z, 1.0F);
    EXPECT_NE(transform.position.x, 0.0F);
    EXPECT_NE(transform.position.y, 0.0F);
}

} // namespace
} // namespace isla::client
