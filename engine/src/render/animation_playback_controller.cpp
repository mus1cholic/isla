#include "engine/src/render/include/animation_playback_controller.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace isla::client::animated_gltf {

namespace {

float sanitize_non_negative_finite(float value, float fallback) {
    if (!std::isfinite(value) || value < 0.0F) {
        return fallback;
    }
    return value;
}

} // namespace

bool AnimationPlaybackController::set_asset(const AnimatedGltfAsset* asset, std::string* error_message) {
    asset_ = asset;
    state_ = ClipPlaybackState{};
    cached_pose_ = EvaluatedPose{};
    has_cached_pose_ = false;

    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is null";
        }
        return false;
    }
    if (asset_->clips.empty()) {
        if (error_message != nullptr) {
            *error_message = "animation asset has no clips";
        }
        return false;
    }
    return evaluate_internal(error_message);
}

void AnimationPlaybackController::clear_asset() {
    asset_ = nullptr;
    state_ = ClipPlaybackState{};
    cached_pose_ = EvaluatedPose{};
    has_cached_pose_ = false;
}

const AnimatedGltfAsset* AnimationPlaybackController::asset() const {
    return asset_;
}

const ClipPlaybackState& AnimationPlaybackController::state() const {
    return state_;
}

const EvaluatedPose& AnimationPlaybackController::cached_pose() const {
    return cached_pose_;
}

bool AnimationPlaybackController::has_cached_pose() const {
    return has_cached_pose_;
}

bool AnimationPlaybackController::set_clip_index(std::size_t clip_index, std::string* error_message) {
    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is not set";
        }
        return false;
    }
    if (clip_index >= asset_->clips.size()) {
        if (error_message != nullptr) {
            *error_message = "clip index out of range";
        }
        return false;
    }
    state_.clip_index = clip_index;
    state_.local_time_seconds = 0.0F;
    return evaluate_internal(error_message);
}

void AnimationPlaybackController::set_playing(bool playing) {
    state_.is_playing = playing;
}

void AnimationPlaybackController::set_playback_mode(ClipPlaybackMode mode) {
    state_.playback_mode = mode;
}

void AnimationPlaybackController::set_speed(float speed) {
    state_.speed = sanitize_non_negative_finite(speed, 1.0F);
}

bool AnimationPlaybackController::seek_local_time(float time_seconds, std::string* error_message) {
    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is not set";
        }
        return false;
    }
    state_.local_time_seconds = sanitize_non_negative_finite(time_seconds, 0.0F);
    return evaluate_internal(error_message);
}

bool AnimationPlaybackController::tick(float frame_dt_seconds, std::string* error_message) {
    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is not set";
        }
        return false;
    }
    const float dt = sanitize_non_negative_finite(frame_dt_seconds, 0.0F);
    if (state_.is_playing && state_.speed > 0.0F) {
        state_.local_time_seconds += dt * state_.speed;
        if (!std::isfinite(state_.local_time_seconds) || state_.local_time_seconds < 0.0F) {
            state_.local_time_seconds = 0.0F;
        }
    }
    return evaluate_internal(error_message);
}

bool AnimationPlaybackController::evaluate_now(std::string* error_message) {
    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is not set";
        }
        return false;
    }
    return evaluate_internal(error_message);
}

bool AnimationPlaybackController::evaluate_internal(std::string* error_message) {
    if (asset_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "animation asset is not set";
        }
        return false;
    }
    EvaluatedPose pose;
    std::string eval_error;
    if (!evaluate_clip_pose(*asset_, state_.clip_index, state_.local_time_seconds, pose, &eval_error,
                            state_.playback_mode)) {
        if (error_message != nullptr) {
            *error_message = eval_error;
        }
        has_cached_pose_ = false;
        return false;
    }
    cached_pose_ = std::move(pose);
    has_cached_pose_ = true;
    return true;
}

} // namespace isla::client::animated_gltf
