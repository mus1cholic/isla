#pragma once

#include <cstddef>
#include <string>

#include "isla/engine/render/animated_gltf.hpp"

namespace isla::client::animated_gltf {

struct ClipPlaybackState {
    std::size_t clip_index = 0U;
    float local_time_seconds = 0.0F;
    bool is_playing = true;
    float speed = 1.0F;
    ClipPlaybackMode playback_mode = ClipPlaybackMode::Loop;
};

// Frame-time playback controller for AnimatedGltfAsset clip evaluation.
// Policy: callers provide per-frame dt; controller advances local clip time and
// refreshes cached pose each tick.
class AnimationPlaybackController {
  public:
    [[nodiscard]] bool set_asset(const AnimatedGltfAsset* asset,
                                 std::string* error_message = nullptr);
    void clear_asset();

    [[nodiscard]] const AnimatedGltfAsset* asset() const;
    [[nodiscard]] const ClipPlaybackState& state() const;
    [[nodiscard]] const EvaluatedPose& cached_pose() const;
    [[nodiscard]] bool has_cached_pose() const;

    [[nodiscard]] bool set_clip_index(std::size_t clip_index, std::string* error_message = nullptr);
    void set_playing(bool playing);
    void set_playback_mode(ClipPlaybackMode mode);
    void set_speed(float speed);
    [[nodiscard]] bool seek_local_time(float time_seconds, std::string* error_message = nullptr);

    [[nodiscard]] bool tick(float frame_dt_seconds, std::string* error_message = nullptr);
    [[nodiscard]] bool evaluate_now(std::string* error_message = nullptr);

  private:
    [[nodiscard]] bool evaluate_internal(std::string* error_message);

    const AnimatedGltfAsset* asset_ = nullptr;
    ClipPlaybackState state_{};
    EvaluatedPose cached_pose_{};
    bool has_cached_pose_ = false;
};

} // namespace isla::client::animated_gltf
