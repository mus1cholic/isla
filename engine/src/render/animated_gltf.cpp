#include "isla/engine/render/animated_gltf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include <cgltf.h>

namespace isla::client::animated_gltf {

namespace {

std::string make_animation_keyframe_error(const AnimationClip& clip, std::size_t anim_index,
                                          std::size_t target_index, const char* path_name,
                                          cgltf_size key_index) {
    std::string message = "failed reading ";
    message += path_name;
    message += " keyframe at key index ";
    message += std::to_string(static_cast<std::size_t>(key_index));
    message += " for animation index ";
    message += std::to_string(anim_index);
    if (!clip.name.empty()) {
        message += " ('";
        message += clip.name;
        message += "')";
    }
    message += ", target index ";
    message += std::to_string(target_index);
    return message;
}

std::string make_animation_count_mismatch_error(const AnimationClip& clip, std::size_t anim_index,
                                                std::size_t target_index, const char* path_name,
                                                cgltf_size input_count, cgltf_size output_count) {
    std::string message = "mismatched animation key counts for ";
    message += path_name;
    message += ": input=";
    message += std::to_string(static_cast<std::size_t>(input_count));
    message += ", output=";
    message += std::to_string(static_cast<std::size_t>(output_count));
    message += ", animation index ";
    message += std::to_string(anim_index);
    if (!clip.name.empty()) {
        message += " ('";
        message += clip.name;
        message += "')";
    }
    message += ", target index ";
    message += std::to_string(target_index);
    return message;
}

class CgltfDataDeleter {
  public:
    void operator()(cgltf_data* data) const {
        if (data != nullptr) {
            cgltf_free(data);
        }
    }
};

using CgltfDataPtr = std::unique_ptr<cgltf_data, CgltfDataDeleter>;

std::optional<Vec3> read_vec3(const cgltf_accessor* accessor, cgltf_size index) {
    if (accessor == nullptr) {
        return std::nullopt;
    }
    std::array<float, 3U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    return Vec3{ .x = values[0], .y = values[1], .z = values[2] };
}

std::optional<Vec2> read_vec2(const cgltf_accessor* accessor, cgltf_size index) {
    if (accessor == nullptr) {
        return std::nullopt;
    }
    std::array<float, 2U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    return Vec2{ .x = values[0], .y = values[1] };
}

std::optional<Quat> read_quat(const cgltf_accessor* accessor, cgltf_size index) {
    if (accessor == nullptr) {
        return std::nullopt;
    }
    std::array<float, 4U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    Quat q{ .x = values[0], .y = values[1], .z = values[2], .w = values[3] };
    q.normalize();
    return q;
}

std::optional<Mat4> read_mat4(const cgltf_accessor* accessor, cgltf_size index) {
    if (accessor == nullptr) {
        return std::nullopt;
    }
    std::array<float, 16U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    Mat4 m{};
    m.elements = values;
    return m;
}

std::optional<float> read_scalar(const cgltf_accessor* accessor, cgltf_size index) {
    if (accessor == nullptr) {
        return std::nullopt;
    }
    std::array<float, 1U> values{};
    if (cgltf_accessor_read_float(accessor, index, values.data(), values.size()) == 0) {
        return std::nullopt;
    }
    return values[0];
}

Mat4 read_node_local_matrix(const cgltf_node* node) {
    Mat4 local_matrix = Mat4::identity();
    if (node == nullptr) {
        return local_matrix;
    }
    std::array<float, 16U> values{};
    cgltf_node_transform_local(node, values.data());
    local_matrix.elements = values;
    return local_matrix;
}

Transform read_node_local_transform(const cgltf_node* node) {
    Transform local{};
    if (node == nullptr) {
        return local;
    }
    if (node->has_translation != 0) {
        local.position = Vec3{
            .x = node->translation[0],
            .y = node->translation[1],
            .z = node->translation[2],
        };
    }
    if (node->has_rotation != 0) {
        local.rotation = Quat{
            .x = node->rotation[0],
            .y = node->rotation[1],
            .z = node->rotation[2],
            .w = node->rotation[3],
        };
        local.rotation.normalize();
    }
    if (node->has_scale != 0) {
        local.scale = Vec3{
            .x = node->scale[0],
            .y = node->scale[1],
            .z = node->scale[2],
        };
    }
    return local;
}

bool read_vec4_u16(const cgltf_accessor* accessor, cgltf_size index,
                   std::array<std::uint16_t, 4U>& out_values) {
    if (accessor == nullptr) {
        return false;
    }
    std::array<cgltf_uint, 4U> values{};
    if (cgltf_accessor_read_uint(accessor, index, values.data(), values.size()) == 0) {
        return false;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        out_values[i] = static_cast<std::uint16_t>(values[i]);
    }
    return true;
}

float normalize_sample_time(float input_time, float duration_seconds, ClipPlaybackMode mode) {
    if (!std::isfinite(input_time)) {
        return 0.0F;
    }
    if (duration_seconds <= 0.0F || !std::isfinite(duration_seconds)) {
        return std::max(0.0F, input_time);
    }
    const float clamped_non_negative = std::max(0.0F, input_time);
    if (mode == ClipPlaybackMode::Clamp) {
        return std::min(clamped_non_negative, duration_seconds);
    }
    float wrapped = std::fmod(clamped_non_negative, duration_seconds);
    if (wrapped < 0.0F) {
        wrapped += duration_seconds;
    }
    return wrapped;
}

Vec3 sample_vec3_track(const std::vector<Vec3Keyframe>& keyframes, float time_seconds,
                       const Vec3& fallback, TrackInterpolation interpolation) {
    if (keyframes.empty()) {
        return fallback;
    }
    if (keyframes.size() == 1U || time_seconds <= keyframes.front().time_seconds) {
        return keyframes.front().value;
    }
    if (time_seconds >= keyframes.back().time_seconds) {
        return keyframes.back().value;
    }
    // TODO(isla): For monotonic playback, cache the last key index per track and use
    // index-walking to reduce sampling to amortized O(1). Keep lower_bound as fallback for seeks.
    const auto upper_it =
        std::lower_bound(keyframes.begin(), keyframes.end(), time_seconds,
                         [](const Vec3Keyframe& key, float t) { return key.time_seconds < t; });
    if (upper_it == keyframes.end()) {
        return keyframes.back().value;
    }
    const std::size_t upper_index = static_cast<std::size_t>(upper_it - keyframes.begin());
    if (upper_index == 0U) {
        return keyframes.front().value;
    }

    const Vec3Keyframe& a = keyframes[upper_index - 1U];
    if (interpolation == TrackInterpolation::Step) {
        if (upper_it != keyframes.end() && upper_it->time_seconds == time_seconds) {
            return upper_it->value;
        }
        return a.value;
    }
    const Vec3Keyframe& b = keyframes[upper_index];
    const float span = b.time_seconds - a.time_seconds;
    if (span <= 1.0e-6F) {
        return a.value;
    }
    const float t = (time_seconds - a.time_seconds) / span;
    return Vec3{
        .x = a.value.x + ((b.value.x - a.value.x) * t),
        .y = a.value.y + ((b.value.y - a.value.y) * t),
        .z = a.value.z + ((b.value.z - a.value.z) * t),
    };
}

Quat sample_quat_track(const std::vector<QuatKeyframe>& keyframes, float time_seconds,
                       const Quat& fallback, TrackInterpolation interpolation) {
    if (keyframes.empty()) {
        return fallback;
    }
    if (keyframes.size() == 1U || time_seconds <= keyframes.front().time_seconds) {
        return keyframes.front().value;
    }
    if (time_seconds >= keyframes.back().time_seconds) {
        return keyframes.back().value;
    }
    // TODO(isla): For monotonic playback, cache the last key index per track and use
    // index-walking to reduce sampling to amortized O(1). Keep lower_bound as fallback for seeks.
    const auto upper_it =
        std::lower_bound(keyframes.begin(), keyframes.end(), time_seconds,
                         [](const QuatKeyframe& key, float t) { return key.time_seconds < t; });
    if (upper_it == keyframes.end()) {
        return keyframes.back().value;
    }
    const std::size_t upper_index = static_cast<std::size_t>(upper_it - keyframes.begin());
    if (upper_index == 0U) {
        return keyframes.front().value;
    }

    const QuatKeyframe& a = keyframes[upper_index - 1U];
    if (interpolation == TrackInterpolation::Step) {
        if (upper_it != keyframes.end() && upper_it->time_seconds == time_seconds) {
            return upper_it->value;
        }
        return a.value;
    }
    const QuatKeyframe& b = keyframes[upper_index];
    const float span = b.time_seconds - a.time_seconds;
    if (span <= 1.0e-6F) {
        return a.value;
    }
    const float t = (time_seconds - a.time_seconds) / span;
    return slerp(a.value, b.value, t);
}

bool has_track_animation(const JointAnimationTrack& track) {
    return !track.translations.empty() || !track.rotations.empty() || !track.scales.empty();
}

void sort_track_keyframes(JointAnimationTrack& track) {
    auto vec3_key_sorter = [](const Vec3Keyframe& a, const Vec3Keyframe& b) {
        return a.time_seconds < b.time_seconds;
    };
    auto quat_key_sorter = [](const QuatKeyframe& a, const QuatKeyframe& b) {
        return a.time_seconds < b.time_seconds;
    };
    std::sort(track.translations.begin(), track.translations.end(), vec3_key_sorter);
    std::sort(track.scales.begin(), track.scales.end(), vec3_key_sorter);
    std::sort(track.rotations.begin(), track.rotations.end(), quat_key_sorter);
}

bool append_channel_samples(AnimationClip& clip, std::size_t anim_index, std::size_t track_index,
                            const cgltf_animation_channel& channel,
                            JointAnimationTrack& primary_track,
                            JointAnimationTrack* secondary_track, std::string& error_message) {
    const cgltf_accessor* input = channel.sampler->input;
    const cgltf_accessor* output = channel.sampler->output;
    TrackInterpolation interpolation = TrackInterpolation::Linear;
    if (channel.sampler->interpolation == cgltf_interpolation_type_step) {
        interpolation = TrackInterpolation::Step;
    } else if (channel.sampler->interpolation == cgltf_interpolation_type_linear) {
        interpolation = TrackInterpolation::Linear;
    } else if (channel.sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
        error_message = "animation interpolation CUBICSPLINE is not supported yet";
        return false;
    } else {
        error_message = "animation interpolation mode is not supported";
        return false;
    }

    const char* path_name = "unknown";
    if (channel.target_path == cgltf_animation_path_type_translation) {
        path_name = "translation";
    } else if (channel.target_path == cgltf_animation_path_type_rotation) {
        path_name = "rotation";
    } else if (channel.target_path == cgltf_animation_path_type_scale) {
        path_name = "scale";
    }
    if (input->count != output->count) {
        error_message = make_animation_count_mismatch_error(clip, anim_index, track_index,
                                                            path_name, input->count, output->count);
        return false;
    }
    const cgltf_size count = input->count;

    auto set_track_interpolation = [&](JointAnimationTrack& track) -> bool {
        if (channel.target_path == cgltf_animation_path_type_translation) {
            if (!track.translations.empty() && track.translation_interpolation != interpolation) {
                error_message =
                    "multiple interpolation modes for one translation track are not supported";
                return false;
            }
            track.translation_interpolation = interpolation;
        } else if (channel.target_path == cgltf_animation_path_type_rotation) {
            if (!track.rotations.empty() && track.rotation_interpolation != interpolation) {
                error_message =
                    "multiple interpolation modes for one rotation track are not supported";
                return false;
            }
            track.rotation_interpolation = interpolation;
        } else if (channel.target_path == cgltf_animation_path_type_scale) {
            if (!track.scales.empty() && track.scale_interpolation != interpolation) {
                error_message = "multiple interpolation modes for one scale track are not "
                                "supported";
                return false;
            }
            track.scale_interpolation = interpolation;
        }
        return true;
    };

    if (!set_track_interpolation(primary_track) ||
        (secondary_track != nullptr && !set_track_interpolation(*secondary_track))) {
        return false;
    }

    if (channel.target_path == cgltf_animation_path_type_translation) {
        primary_track.translations.reserve(primary_track.translations.size() +
                                           static_cast<std::size_t>(count));
        if (secondary_track != nullptr) {
            secondary_track->translations.reserve(secondary_track->translations.size() +
                                                  static_cast<std::size_t>(count));
        }
        for (cgltf_size i = 0U; i < count; ++i) {
            const std::optional<float> t = read_scalar(input, i);
            const std::optional<Vec3> v = read_vec3(output, i);
            if (!t.has_value() || !v.has_value()) {
                error_message =
                    make_animation_keyframe_error(clip, anim_index, track_index, path_name, i);
                return false;
            }
            clip.duration_seconds = std::max(clip.duration_seconds, *t);
            const Vec3Keyframe keyframe{ .time_seconds = *t, .value = *v };
            primary_track.translations.push_back(keyframe);
            if (secondary_track != nullptr) {
                secondary_track->translations.push_back(keyframe);
            }
        }
        return true;
    }

    if (channel.target_path == cgltf_animation_path_type_rotation) {
        primary_track.rotations.reserve(primary_track.rotations.size() +
                                        static_cast<std::size_t>(count));
        if (secondary_track != nullptr) {
            secondary_track->rotations.reserve(secondary_track->rotations.size() +
                                               static_cast<std::size_t>(count));
        }
        for (cgltf_size i = 0U; i < count; ++i) {
            const std::optional<float> t = read_scalar(input, i);
            const std::optional<Quat> q = read_quat(output, i);
            if (!t.has_value() || !q.has_value()) {
                error_message =
                    make_animation_keyframe_error(clip, anim_index, track_index, path_name, i);
                return false;
            }
            clip.duration_seconds = std::max(clip.duration_seconds, *t);
            const QuatKeyframe keyframe{ .time_seconds = *t, .value = *q };
            primary_track.rotations.push_back(keyframe);
            if (secondary_track != nullptr) {
                secondary_track->rotations.push_back(keyframe);
            }
        }
        return true;
    }

    if (channel.target_path == cgltf_animation_path_type_scale) {
        primary_track.scales.reserve(primary_track.scales.size() + static_cast<std::size_t>(count));
        if (secondary_track != nullptr) {
            secondary_track->scales.reserve(secondary_track->scales.size() +
                                            static_cast<std::size_t>(count));
        }
        for (cgltf_size i = 0U; i < count; ++i) {
            const std::optional<float> t = read_scalar(input, i);
            const std::optional<Vec3> v = read_vec3(output, i);
            if (!t.has_value() || !v.has_value()) {
                error_message =
                    make_animation_keyframe_error(clip, anim_index, track_index, path_name, i);
                return false;
            }
            clip.duration_seconds = std::max(clip.duration_seconds, *t);
            const Vec3Keyframe keyframe{ .time_seconds = *t, .value = *v };
            primary_track.scales.push_back(keyframe);
            if (secondary_track != nullptr) {
                secondary_track->scales.push_back(keyframe);
            }
        }
        return true;
    }

    return true;
}

} // namespace

AnimatedNodeSummary summarize_animated_nodes(const AnimatedGltfAsset& asset) {
    AnimatedNodeSummary summary{};
    if (asset.nodes.empty()) {
        return summary;
    }

    std::vector<bool> joint_node_flags(asset.nodes.size(), false);
    for (const std::size_t joint_node_index : asset.joint_node_indices) {
        if (joint_node_index < joint_node_flags.size()) {
            joint_node_flags[joint_node_index] = true;
        }
    }

    std::vector<bool> animated_flags(asset.nodes.size(), false);
    for (const AnimationClip& clip : asset.clips) {
        const std::size_t track_count = std::min(asset.nodes.size(), clip.node_tracks.size());
        for (std::size_t node_index = 0U; node_index < track_count; ++node_index) {
            if (animated_flags[node_index] || !has_track_animation(clip.node_tracks[node_index])) {
                continue;
            }
            animated_flags[node_index] = true;
            ++summary.animated_nodes;
            if (!joint_node_flags[node_index]) {
                ++summary.animated_non_joint_nodes;
            }
        }
    }
    return summary;
}

AnimatedGltfLoadResult load_from_file(std::string_view asset_path) {
    const std::filesystem::path path(asset_path);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".gltf" && extension != ".glb") {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "animated glTF loader supports only .gltf/.glb",
        };
    }

    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    if (cgltf_parse_file(&options, std::string(asset_path).c_str(), &parsed) !=
            cgltf_result_success ||
        parsed == nullptr) {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "failed to parse glTF/GLB file",
        };
    }
    CgltfDataPtr data(parsed);

    if (cgltf_load_buffers(&options, data.get(), std::string(asset_path).c_str()) !=
        cgltf_result_success) {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "failed to load glTF buffers",
        };
    }

    if (data->skins_count == 0U) {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "glTF has no skin",
        };
    }
    const cgltf_skin& skin = data->skins[0];
    if (skin.joints_count == 0U) {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "glTF skin has no joints",
        };
    }

    AnimatedGltfAsset asset;
    asset.nodes.resize(static_cast<std::size_t>(data->nodes_count));
    asset.skeleton.joints.resize(static_cast<std::size_t>(skin.joints_count));
    asset.bind_local_transforms.resize(static_cast<std::size_t>(skin.joints_count));
    asset.joint_node_indices.resize(static_cast<std::size_t>(skin.joints_count));

    std::unordered_map<const cgltf_node*, std::size_t> node_index_by_ptr;
    node_index_by_ptr.reserve(static_cast<std::size_t>(data->nodes_count));
    for (cgltf_size i = 0U; i < data->nodes_count; ++i) {
        node_index_by_ptr.emplace(&data->nodes[i], static_cast<std::size_t>(i));
    }

    for (cgltf_size i = 0U; i < data->nodes_count; ++i) {
        const cgltf_node& source_node = data->nodes[i];
        AnimatedNode& node = asset.nodes[static_cast<std::size_t>(i)];
        node.name = source_node.name != nullptr ? source_node.name : "";
        node.parent_index = -1;
        if (source_node.parent != nullptr) {
            const auto parent_it = node_index_by_ptr.find(source_node.parent);
            if (parent_it == node_index_by_ptr.end()) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "glTF node parent could not be resolved",
                };
            }
            node.parent_index = static_cast<int>(parent_it->second);
        }
        node.uses_trs = (source_node.has_matrix == 0);
        node.bind_local_matrix = read_node_local_matrix(&source_node);
        if (node.uses_trs) {
            node.bind_local_transform = read_node_local_transform(&source_node);
        }
    }

    std::unordered_map<const cgltf_node*, std::size_t> joint_index_by_node;
    joint_index_by_node.reserve(static_cast<std::size_t>(skin.joints_count));
    for (cgltf_size i = 0U; i < skin.joints_count; ++i) {
        joint_index_by_node.emplace(skin.joints[i], static_cast<std::size_t>(i));
    }

    for (cgltf_size i = 0U; i < skin.joints_count; ++i) {
        const cgltf_node* node = skin.joints[i];
        SkeletonJoint& joint = asset.skeleton.joints[static_cast<std::size_t>(i)];
        joint.name = (node != nullptr && node->name != nullptr) ? node->name : "";
        joint.parent_index = -1;
        const auto node_it = node_index_by_ptr.find(node);
        if (node_it == node_index_by_ptr.end()) {
            return AnimatedGltfLoadResult{
                .ok = false,
                .asset = {},
                .error_message = "glTF skin joint node could not be resolved",
            };
        }
        const std::size_t node_index = node_it->second;
        asset.joint_node_indices[static_cast<std::size_t>(i)] = node_index;

        const cgltf_node* ancestor = (node != nullptr) ? node->parent : nullptr;
        cgltf_size ancestry_hops = 0U;
        while (ancestor != nullptr) {
            const auto parent_it = joint_index_by_node.find(ancestor);
            if (parent_it != joint_index_by_node.end()) {
                joint.parent_index = static_cast<int>(parent_it->second);
                break;
            }
            ancestor = ancestor->parent;
            ++ancestry_hops;
            if (ancestry_hops > data->nodes_count) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "glTF node hierarchy contains a parent cycle",
                };
            }
        }
        if (skin.inverse_bind_matrices != nullptr) {
            const std::optional<Mat4> ibm = read_mat4(skin.inverse_bind_matrices, i);
            if (!ibm.has_value()) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "failed reading inverse bind matrix",
                };
            }
            joint.inverse_bind_matrix = *ibm;
        }

        const AnimatedNode& bind_node = asset.nodes[node_index];
        if (!bind_node.uses_trs) {
            LOG(ERROR) << "AnimatedGltfLoader: matrix-authored joint node encountered at joint "
                       << "index " << static_cast<std::size_t>(i)
                       << "; only TRS-authored joints are supported currently";
            return AnimatedGltfLoadResult{
                .ok = false,
                .asset = {},
                .error_message =
                    "matrix-authored joint nodes are not supported yet; use TRS-authored joints",
            };
        }
        asset.bind_local_transforms[static_cast<std::size_t>(i)] = bind_node.bind_local_transform;
    }

    std::vector<const cgltf_primitive*> skin_primitives;
    skin_primitives.reserve(4U);
    std::unordered_set<const cgltf_primitive*> seen_skin_primitives;

    for (cgltf_size node_i = 0U; node_i < data->nodes_count; ++node_i) {
        const cgltf_node& node = data->nodes[node_i];
        if (node.skin != &skin || node.mesh == nullptr) {
            continue;
        }
        // TODO(isla): Keep per-node primitive instances (including node-local transforms)
        // instead of deduplicating primitive pointers.
        for (cgltf_size p = 0U; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive* primitive = &node.mesh->primitives[p];
            if (primitive->type != cgltf_primitive_type_triangles) {
                continue;
            }
            if (seen_skin_primitives.insert(primitive).second) {
                skin_primitives.push_back(primitive);
            }
        }
    }

    if (skin_primitives.empty()) {
        return AnimatedGltfLoadResult{
            .ok = false,
            .asset = {},
            .error_message = "glTF has no triangle primitive attached to selected skin",
        };
    }

    for (const cgltf_primitive* primitive : skin_primitives) {
        const cgltf_accessor* position_accessor = nullptr;
        const cgltf_accessor* normal_accessor = nullptr;
        const cgltf_accessor* uv_accessor = nullptr;
        const cgltf_accessor* joints_accessor = nullptr;
        const cgltf_accessor* weights_accessor = nullptr;

        for (cgltf_size i = 0U; i < primitive->attributes_count; ++i) {
            const cgltf_attribute& attr = primitive->attributes[i];
            if (attr.data == nullptr) {
                continue;
            }
            if (attr.type == cgltf_attribute_type_position) {
                position_accessor = attr.data;
            } else if (attr.type == cgltf_attribute_type_normal) {
                normal_accessor = attr.data;
            } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0U) {
                uv_accessor = attr.data;
            } else if (attr.type == cgltf_attribute_type_joints && attr.index == 0U) {
                joints_accessor = attr.data;
            } else if (attr.type == cgltf_attribute_type_weights && attr.index == 0U) {
                weights_accessor = attr.data;
            }
        }

        if (position_accessor == nullptr || joints_accessor == nullptr ||
            weights_accessor == nullptr) {
            return AnimatedGltfLoadResult{
                .ok = false,
                .asset = {},
                .error_message = "glTF skinned primitive missing POSITION/JOINTS_0/WEIGHTS_0",
            };
        }

        const cgltf_size vertex_count = position_accessor->count;
        if (vertex_count == 0U) {
            return AnimatedGltfLoadResult{
                .ok = false,
                .asset = {},
                .error_message = "glTF skinned primitive has zero vertices",
            };
        }

        SkinnedPrimitive skinned_primitive;
        skinned_primitive.vertices.resize(static_cast<std::size_t>(vertex_count));
        for (cgltf_size i = 0U; i < vertex_count; ++i) {
            const std::optional<Vec3> pos = read_vec3(position_accessor, i);
            if (!pos.has_value()) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "failed reading POSITION",
                };
            }
            SkinnedVertex v{};
            v.position = *pos;
            if (normal_accessor != nullptr) {
                const std::optional<Vec3> n = read_vec3(normal_accessor, i);
                if (n.has_value()) {
                    v.normal = *n;
                }
            }
            if (uv_accessor != nullptr) {
                const std::optional<Vec2> uv = read_vec2(uv_accessor, i);
                if (uv.has_value()) {
                    v.uv = *uv;
                }
            }

            if (!read_vec4_u16(joints_accessor, i, v.joints)) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "failed reading JOINTS_0",
                };
            }
            std::array<float, 4U> weights{};
            if (cgltf_accessor_read_float(weights_accessor, i, weights.data(), weights.size()) ==
                0) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "failed reading WEIGHTS_0",
                };
            }
            float sum = 0.0F;
            for (std::size_t w = 0; w < weights.size(); ++w) {
                v.weights[w] = std::max(0.0F, weights[w]);
                sum += v.weights[w];
                if (v.joints[w] >= asset.skeleton.joints.size()) {
                    return AnimatedGltfLoadResult{
                        .ok = false,
                        .asset = {},
                        .error_message = "JOINTS_0 index out of range for skin",
                    };
                }
            }
            if (sum > 1.0e-6F) {
                for (float& w : v.weights) {
                    w /= sum;
                }
            } else {
                v.weights = { 1.0F, 0.0F, 0.0F, 0.0F };
                v.joints = { 0U, 0U, 0U, 0U };
            }
            skinned_primitive.vertices[static_cast<std::size_t>(i)] = v;
        }

        if (primitive->indices != nullptr) {
            skinned_primitive.indices.reserve(static_cast<std::size_t>(primitive->indices->count));
            for (cgltf_size i = 0U; i < primitive->indices->count; ++i) {
                const cgltf_size idx = cgltf_accessor_read_index(primitive->indices, i);
                if (idx >= vertex_count || idx > std::numeric_limits<std::uint32_t>::max()) {
                    return AnimatedGltfLoadResult{
                        .ok = false,
                        .asset = {},
                        .error_message = "index out of range",
                    };
                }
                skinned_primitive.indices.push_back(static_cast<std::uint32_t>(idx));
            }
        } else {
            skinned_primitive.indices.resize(static_cast<std::size_t>(vertex_count));
            for (cgltf_size i = 0U; i < vertex_count; ++i) {
                skinned_primitive.indices[static_cast<std::size_t>(i)] =
                    static_cast<std::uint32_t>(i);
            }
        }

        asset.primitives.push_back(std::move(skinned_primitive));
    }

    asset.clips.resize(static_cast<std::size_t>(data->animations_count));
    for (cgltf_size anim_i = 0U; anim_i < data->animations_count; ++anim_i) {
        const cgltf_animation& anim = data->animations[anim_i];
        AnimationClip& clip = asset.clips[static_cast<std::size_t>(anim_i)];
        clip.name = anim.name != nullptr ? anim.name : "";
        clip.joint_tracks.resize(asset.skeleton.joints.size());
        clip.node_tracks.resize(asset.nodes.size());
        clip.duration_seconds = 0.0F;

        for (cgltf_size ch_i = 0U; ch_i < anim.channels_count; ++ch_i) {
            const cgltf_animation_channel& channel = anim.channels[ch_i];
            if (channel.target_node == nullptr || channel.sampler == nullptr ||
                channel.sampler->input == nullptr || channel.sampler->output == nullptr) {
                continue;
            }
            const auto node_it = node_index_by_ptr.find(channel.target_node);
            if (node_it == node_index_by_ptr.end()) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = "animation target node could not be resolved",
                };
            }
            const std::size_t node_index = node_it->second;
            const AnimatedNode& bind_node = asset.nodes[node_index];
            if (!bind_node.uses_trs) {
                LOG(ERROR) << "AnimatedGltfLoader: matrix-authored animated node encountered at "
                           << "node index " << node_index << " name='"
                           << (bind_node.name.empty() ? std::string("<unnamed>") : bind_node.name)
                           << "'; only TRS-authored animated nodes are supported currently";
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message =
                        "matrix-authored animated nodes are not supported yet; use TRS-authored "
                        "animated nodes",
                };
            }
            const auto joint_it = joint_index_by_node.find(channel.target_node);
            JointAnimationTrack* joint_track = joint_it != joint_index_by_node.end()
                                                   ? &clip.joint_tracks[joint_it->second]
                                                   : nullptr;
            std::string channel_error;
            if (!append_channel_samples(clip, static_cast<std::size_t>(anim_i), node_index, channel,
                                        clip.node_tracks[node_index], joint_track, channel_error)) {
                return AnimatedGltfLoadResult{
                    .ok = false,
                    .asset = {},
                    .error_message = std::move(channel_error),
                };
            }
        }

        for (JointAnimationTrack& track : clip.joint_tracks) {
            sort_track_keyframes(track);
        }
        for (JointAnimationTrack& track : clip.node_tracks) {
            sort_track_keyframes(track);
        }
    }

    const AnimatedNodeSummary node_summary = summarize_animated_nodes(asset);
    VLOG(1) << "AnimatedGltfLoader: loaded asset nodes=" << asset.nodes.size()
            << " joints=" << asset.skeleton.joints.size() << " clips=" << asset.clips.size()
            << " primitives=" << asset.primitives.size()
            << " animated_nodes=" << node_summary.animated_nodes
            << " animated_non_joint_nodes=" << node_summary.animated_non_joint_nodes;

    return AnimatedGltfLoadResult{
        .ok = true,
        .asset = std::move(asset),
        .error_message = {},
    };
}

bool evaluate_clip_pose(const AnimatedGltfAsset& asset, std::size_t clip_index, float time_seconds,
                        EvaluatedPose& out_pose, std::string* error_message,
                        ClipPlaybackMode playback_mode) {
    if (clip_index >= asset.clips.size()) {
        if (error_message != nullptr) {
            *error_message = "clip index out of range";
        }
        return false;
    }
    const std::size_t joint_count = asset.skeleton.joints.size();
    const bool has_explicit_nodes = !asset.nodes.empty();
    if (joint_count == 0U ||
        (!has_explicit_nodes && asset.bind_local_transforms.size() != joint_count) ||
        (has_explicit_nodes && asset.joint_node_indices.size() != joint_count)) {
        if (error_message != nullptr) {
            *error_message = "asset skeleton/bind transforms are inconsistent";
        }
        return false;
    }

    const AnimationClip& clip = asset.clips[clip_index];
    const float sample_time =
        normalize_sample_time(time_seconds, clip.duration_seconds, playback_mode);

    out_pose.global_joint_matrices.assign(joint_count, Mat4::identity());
    out_pose.skin_matrices.assign(joint_count, Mat4::identity());
    const std::size_t node_count = has_explicit_nodes ? asset.nodes.size() : joint_count;
    std::vector<Mat4> local_node_matrices(node_count, Mat4::identity());
    std::vector<Mat4> global_node_matrices(node_count, Mat4::identity());

    for (std::size_t node_index = 0U; node_index < node_count; ++node_index) {
        bool uses_trs = true;
        Transform bind_local{};
        Mat4 bind_local_matrix = Mat4::identity();
        if (has_explicit_nodes) {
            const AnimatedNode& node = asset.nodes[node_index];
            uses_trs = node.uses_trs;
            bind_local = node.bind_local_transform;
            bind_local_matrix = node.bind_local_matrix;
        } else {
            bind_local = asset.bind_local_transforms[node_index];
            bind_local_matrix = Mat4::from_position_scale_quat(
                bind_local.position, bind_local.scale, bind_local.rotation);
        }

        const JointAnimationTrack* track = nullptr;
        if (has_explicit_nodes) {
            track = node_index < clip.node_tracks.size() ? &clip.node_tracks[node_index] : nullptr;
        } else {
            track =
                node_index < clip.joint_tracks.size() ? &clip.joint_tracks[node_index] : nullptr;
        }

        if (!uses_trs) {
            if (track != nullptr && has_track_animation(*track)) {
                if (error_message != nullptr) {
                    *error_message =
                        "matrix-authored animated nodes are not supported yet; use TRS-authored "
                        "animated nodes";
                }
                return false;
            }
            local_node_matrices[node_index] = bind_local_matrix;
            continue;
        }

        const Vec3 local_translation =
            track != nullptr
                ? sample_vec3_track(track->translations, sample_time, bind_local.position,
                                    track->translation_interpolation)
                : bind_local.position;
        const Quat local_rotation =
            track != nullptr ? sample_quat_track(track->rotations, sample_time, bind_local.rotation,
                                                 track->rotation_interpolation)
                             : bind_local.rotation;
        const Vec3 local_scale =
            track != nullptr ? sample_vec3_track(track->scales, sample_time, bind_local.scale,
                                                 track->scale_interpolation)
                             : bind_local.scale;
        local_node_matrices[node_index] =
            Mat4::from_position_scale_quat(local_translation, local_scale, local_rotation);
    }

    enum class EvalState : std::uint8_t {
        Unvisited = 0,
        Visiting,
        Done,
    };
    std::vector<EvalState> eval_states(node_count, EvalState::Unvisited);

    std::function<bool(std::size_t)> evaluate_node = [&](std::size_t node_index) -> bool {
        EvalState& state = eval_states[node_index];
        if (state == EvalState::Done) {
            return true;
        }
        if (state == EvalState::Visiting) {
            if (error_message != nullptr) {
                *error_message = "node hierarchy contains parent cycle";
            }
            return false;
        }

        state = EvalState::Visiting;
        const Mat4& local_matrix = local_node_matrices[node_index];
        const int parent = has_explicit_nodes ? asset.nodes[node_index].parent_index
                                              : asset.skeleton.joints[node_index].parent_index;
        if (parent >= 0) {
            if (static_cast<std::size_t>(parent) >= node_count) {
                if (error_message != nullptr) {
                    *error_message = has_explicit_nodes ? "node parent index out of range"
                                                        : "skeleton parent index out of range";
                }
                return false;
            }
            if (!evaluate_node(static_cast<std::size_t>(parent))) {
                return false;
            }
            global_node_matrices[node_index] =
                multiply(global_node_matrices[static_cast<std::size_t>(parent)], local_matrix);
        } else {
            global_node_matrices[node_index] = local_matrix;
        }
        state = EvalState::Done;
        return true;
    };

    for (std::size_t node_index = 0U; node_index < node_count; ++node_index) {
        if (!evaluate_node(node_index)) {
            return false;
        }
    }

    for (std::size_t joint_index = 0U; joint_index < joint_count; ++joint_index) {
        const std::size_t joint_node_index =
            has_explicit_nodes ? asset.joint_node_indices[joint_index] : joint_index;
        if (joint_node_index >= global_node_matrices.size()) {
            if (error_message != nullptr) {
                *error_message = "joint node index out of range";
            }
            return false;
        }
        out_pose.global_joint_matrices[joint_index] = global_node_matrices[joint_node_index];
        out_pose.skin_matrices[joint_index] =
            multiply(out_pose.global_joint_matrices[joint_index],
                     asset.skeleton.joints[joint_index].inverse_bind_matrix);
    }

    return true;
}

} // namespace isla::client::animated_gltf
