#include "client_app.hpp"

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdlib>
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

} // namespace
} // namespace isla::client
