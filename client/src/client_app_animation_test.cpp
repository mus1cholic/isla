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
    [[nodiscard]] bool set_window_relative_mouse_mode(SDL_Window* window, bool enabled) const override {
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
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };

    animated_gltf::AnimationClip walk;
    walk.name = "walk";
    walk.duration_seconds = 1.0F;
    walk.joint_tracks.resize(1U);
    walk.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F, .value = Vec3{ .x = 10.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F, .value = Vec3{ .x = 12.0F, .y = 0.0F, .z = 0.0F } },
    };

    asset.clips.push_back(std::move(idle));
    asset.clips.push_back(std::move(walk));
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
