#include "client_app.hpp"

#include "absl/log/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "animated_mesh_skinning.hpp"
#include "engine/src/render/include/mesh_asset_loader.hpp"
#include "engine/src/render/include/model_renderer_skinning_utils.hpp"
#include "win32_layered_overlay.hpp"

namespace isla::client {

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint32_t kAnimatedBoundsRecomputeIntervalTicks = 30U;
constexpr char kMeshAssetEnvVar[] = "ISLA_MESH_ASSET";
constexpr char kAnimatedGltfAssetEnvVar[] = "ISLA_ANIMATED_GLTF_ASSET";
constexpr char kAnimationClipEnvVar[] = "ISLA_ANIM_CLIP";
constexpr char kAnimationPlaybackModeEnvVar[] = "ISLA_ANIM_PLAYBACK_MODE";
constexpr char kDefaultStartupModelPath[] = "models/model.glb";

const char* material_blend_mode_name(MaterialBlendMode mode) {
    switch (mode) {
    case MaterialBlendMode::Opaque:
        return "opaque";
    case MaterialBlendMode::AlphaBlend:
        return "alpha_blend";
    }
    return "unknown";
}

const char* material_cull_mode_name(MaterialCullMode mode) {
    switch (mode) {
    case MaterialCullMode::Clockwise:
        return "clockwise";
    case MaterialCullMode::CounterClockwise:
        return "counter_clockwise";
    case MaterialCullMode::Disabled:
        return "disabled";
    }
    return "unknown";
}

Transform make_visible_object_transform(const MeshData& mesh) {
    Transform transform{};
    const BoundingSphere bounds = mesh.local_bounds();
    if (!std::isfinite(bounds.center.x) || !std::isfinite(bounds.center.y) ||
        !std::isfinite(bounds.center.z) || !std::isfinite(bounds.radius) || bounds.radius <= 0.0F) {
        return transform;
    }

    constexpr float kTargetRadius = 1.0F;
    const float scale = kTargetRadius / std::max(bounds.radius, 1.0e-4F);
    transform.scale = Vec3{ .x = scale, .y = scale, .z = scale };
    transform.position = Vec3{ .x = -bounds.center.x * scale,
                               .y = -bounds.center.y * scale,
                               .z = -bounds.center.z * scale };
    return transform;
}

Transform make_visible_object_transform_for_meshes(std::span<const MeshData> meshes) {
    MeshData aggregate_mesh;
    MeshData::TriangleList aggregate_triangles;
    std::size_t aggregate_triangle_count = 0U;
    for (const MeshData& mesh : meshes) {
        aggregate_triangle_count += mesh.triangles().size();
    }
    if (aggregate_triangle_count == 0U) {
        return {};
    }

    aggregate_triangles.reserve(aggregate_triangle_count);
    for (const MeshData& mesh : meshes) {
        aggregate_triangles.insert(aggregate_triangles.end(), mesh.triangles().begin(),
                                   mesh.triangles().end());
    }
    aggregate_mesh.set_triangles(std::move(aggregate_triangles));
    return make_visible_object_transform(aggregate_mesh);
}

std::string resolve_default_startup_model_path() {
    const std::filesystem::path relative_path(kDefaultStartupModelPath);
    std::vector<std::filesystem::path> candidates = { relative_path };
    std::error_code cwd_error;
    const std::filesystem::path current_dir = std::filesystem::current_path(cwd_error);
    if (!cwd_error) {
        candidates.insert(candidates.begin(), current_dir / relative_path);
    }
    const char* workspace_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    if (workspace_dir != nullptr && workspace_dir[0] != '\0') {
        candidates.push_back(std::filesystem::path(workspace_dir) / relative_path);
    }
    for (const std::filesystem::path& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.lexically_normal().string();
        }
    }
    return {};
}

std::size_t find_clip_index_by_name(const animated_gltf::AnimatedGltfAsset& asset,
                                    const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return 0U;
    }
    const auto it =
        std::ranges::find_if(asset.clips, [name](const auto& clip) { return clip.name == name; });
    return static_cast<std::size_t>(std::distance(asset.clips.cbegin(), it));
}

std::vector<SkinnedMeshVertex>
make_render_skinned_vertices(const animated_gltf::SkinnedPrimitive& primitive) {
    std::vector<SkinnedMeshVertex> vertices;
    vertices.reserve(primitive.vertices.size());
    for (const animated_gltf::SkinnedVertex& vertex : primitive.vertices) {
        vertices.push_back(SkinnedMeshVertex{
            .position = vertex.position,
            .normal = vertex.normal,
            .uv = vertex.uv,
            .joints = vertex.joints,
            .weights = vertex.weights,
        });
    }
    return vertices;
}

std::vector<Triangle>
make_triangles_from_skinned_geometry(std::span<const SkinnedMeshVertex> vertices,
                                     std::span<const std::uint32_t> indices) {
    std::vector<Triangle> triangles;
    if ((indices.size() % 3U) != 0U) {
        return triangles;
    }
    triangles.reserve(indices.size() / 3U);
    for (std::size_t i = 0U; i < indices.size(); i += 3U) {
        const std::uint32_t ia = indices[i];
        const std::uint32_t ib = indices[i + 1U];
        const std::uint32_t ic = indices[i + 2U];
        if (static_cast<std::size_t>(ia) >= vertices.size() ||
            static_cast<std::size_t>(ib) >= vertices.size() ||
            static_cast<std::size_t>(ic) >= vertices.size()) {
            continue;
        }
        const SkinnedMeshVertex& a = vertices[ia];
        const SkinnedMeshVertex& b = vertices[ib];
        const SkinnedMeshVertex& c = vertices[ic];
        triangles.push_back(Triangle{
            .a = a.position,
            .b = b.position,
            .c = c.position,
            .uv_a = a.uv,
            .uv_b = b.uv,
            .uv_c = c.uv,
        });
    }
    return triangles;
}

std::vector<Mat4> make_remapped_skin_palette(std::span<const Mat4> global_skin_matrices,
                                             std::span<const std::uint16_t> global_joints) {
    std::vector<Mat4> palette(global_joints.size(), Mat4::identity());
    for (std::size_t local_joint = 0U; local_joint < global_joints.size(); ++local_joint) {
        const std::size_t global_joint = global_joints[local_joint];
        if (global_joint >= global_skin_matrices.size()) {
            LOG_EVERY_N_SEC(WARNING, 2.0)
                << "ClientApp: remapped GPU palette references out-of-range global joint index "
                << global_joint << " for skin matrix count " << global_skin_matrices.size()
                << "; using identity";
            continue;
        }
        palette[local_joint] = global_skin_matrices[global_joint];
    }
    return palette;
}

} // namespace

ClientApp::ClientApp() : ClientApp(default_sdl_runtime()) {}

ClientApp::ClientApp(const ISdlRuntime& sdl_runtime) : sdl_runtime_(sdl_runtime) {}

int ClientApp::run() {
    if (!initialize()) {
        shutdown();
        return 1;
    }

    while (is_running_) {
        tick();
        render();
    }

    shutdown();
    return 0;
}

bool ClientApp::initialize() {
    if (!sdl_runtime_.init_video()) {
        LOG(ERROR) << "ClientApp: SDL video init failed: " << SDL_GetError();
        return false;
    }

    window_ = sdl_runtime_.create_window("isla_overlay", window_width_, window_height_,
                                         SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        LOG(ERROR) << "ClientApp: SDL window creation failed: " << SDL_GetError();
        return false;
    }

    sdl_runtime_.maximize_window(window_);
    if (!configure_win32_layered_overlay(window_)) {
        LOG(WARNING)
            << "ClientApp: layered overlay mode not applied (non-Windows or HWND lookup failure).";
    }

    int pixel_width = window_width_;
    int pixel_height = window_height_;
    if (sdl_runtime_.get_window_size_in_pixels(window_, &pixel_width, &pixel_height)) {
        window_width_ = pixel_width;
        window_height_ = pixel_height;
    }

    if (!model_renderer_.initialize(
            window_, renderer_, RenderSize{ .width = window_width_, .height = window_height_ })) {
        LOG(ERROR) << "ClientApp: model renderer initialize failed";
        return false;
    }
    load_startup_mesh();
    last_tick_ns_ = sdl_runtime_.get_ticks_ns();
    is_running_ = true;
    return true;
}

void ClientApp::load_startup_mesh() {
    world_.materials().clear();
    world_.materials().push_back(Material{});
    world_.meshes().clear();
    world_.objects().clear();
    animated_asset_.reset();
    animation_playback_.clear_asset();
    animated_mesh_bindings_.clear();
    animation_tick_count_ = 0U;
    gpu_skinning_authoritative_ = model_renderer_.supports_gpu_skinning();
    LOG(INFO) << "ClientApp: GPU skinning authoritative mode "
              << (gpu_skinning_authoritative_ ? "enabled" : "disabled")
              << " (renderer support check)";

    const auto try_load_static_asset = [&](const std::string& path,
                                           const char* source_label) -> bool {
        mesh_asset_loader::MeshAssetLoadResult loaded = mesh_asset_loader::load_from_file(path);
        if (!loaded.ok) {
            LOG(WARNING) << "ClientApp: static mesh load failed for " << source_label << "='"
                         << path << "' error='" << loaded.error_message << "'";
            return false;
        }

        std::vector<mesh_asset_loader::MeshAssetPrimitive> primitive_chunks =
            std::move(loaded.primitives);
        if (primitive_chunks.empty() && !loaded.triangles.empty()) {
            primitive_chunks.push_back(mesh_asset_loader::MeshAssetPrimitive{
                .triangles = std::move(loaded.triangles),
                .material = loaded.material,
            });
        }

        world_.materials().clear();
        world_.meshes().clear();
        world_.objects().clear();
        std::size_t total_triangle_count = 0U;
        for (std::size_t chunk_index = 0U; chunk_index < primitive_chunks.size(); ++chunk_index) {
            mesh_asset_loader::MeshAssetPrimitive& chunk = primitive_chunks[chunk_index];
            if (chunk.triangles.empty()) {
                VLOG(1) << "ClientApp: skipping empty static primitive chunk index=" << chunk_index
                        << " from " << source_label << "='" << path << "'";
                continue;
            }
            Material material{};
            material.base_color = chunk.material.base_color;
            material.base_alpha = chunk.material.base_alpha;
            material.alpha_cutoff = chunk.material.alpha_cutoff;
            material.albedo_texture_path = chunk.material.albedo_texture_path;
            material.blend_mode = chunk.material.blend_mode;
            material.cull_mode = chunk.material.cull_mode;
            VLOG(1) << "ClientApp: static primitive chunk index=" << chunk_index
                    << " triangles=" << chunk.triangles.size() << " material={base_color=["
                    << material.base_color.r << "," << material.base_color.g << ","
                    << material.base_color.b << "], base_alpha=" << material.base_alpha
                    << ", alpha_cutoff=" << material.alpha_cutoff
                    << ", blend_mode=" << material_blend_mode_name(material.blend_mode)
                    << ", cull_mode=" << material_cull_mode_name(material.cull_mode)
                    << ", has_albedo_texture="
                    << (!material.albedo_texture_path.empty() ? "true" : "false") << "}";
            if (material.alpha_cutoff >= 0.0F && material.albedo_texture_path.empty()) {
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "ClientApp: static primitive chunk index=" << chunk_index
                    << " uses MASK-like alpha cutoff without albedo texture path; cutout "
                       "appearance may degrade";
            }
            world_.materials().push_back(std::move(material));

            MeshData mesh;
            total_triangle_count += chunk.triangles.size();
            mesh.set_triangles(std::move(chunk.triangles));
            world_.meshes().push_back(std::move(mesh));
            const std::size_t mesh_id = world_.meshes().size() - 1U;
            const std::size_t material_id = world_.materials().size() - 1U;
            world_.objects().push_back(RenderObject{
                .mesh_id = mesh_id,
                .material_id = material_id,
                .visible = true,
            });
        }
        if (world_.meshes().empty()) {
            LOG(WARNING) << "ClientApp: static mesh load produced no renderable primitive chunks "
                         << "for " << source_label << "='" << path << "'";
            return false;
        }
        const Transform aggregate_transform =
            make_visible_object_transform_for_meshes(world_.meshes());
        VLOG(1) << "ClientApp: static aggregate transform applied to " << world_.objects().size()
                << " object(s) position=[" << aggregate_transform.position.x << ","
                << aggregate_transform.position.y << "," << aggregate_transform.position.z
                << "] scale=[" << aggregate_transform.scale.x << "," << aggregate_transform.scale.y
                << "," << aggregate_transform.scale.z << "]";
        for (RenderObject& object : world_.objects()) {
            object.transform = aggregate_transform;
        }

        const Material& first_material = world_.materials().front();
        LOG(INFO) << "ClientApp: loaded static mesh from " << source_label << "='" << path
                  << "' triangles=" << total_triangle_count
                  << " primitive_meshes=" << world_.meshes().size()
                  << " materials=" << world_.materials().size() << " first_material={base_color=["
                  << first_material.base_color.r << "," << first_material.base_color.g << ","
                  << first_material.base_color.b << "], base_alpha=" << first_material.base_alpha
                  << ", alpha_cutoff=" << first_material.alpha_cutoff
                  << ", blend_mode=" << material_blend_mode_name(first_material.blend_mode)
                  << ", cull_mode=" << material_cull_mode_name(first_material.cull_mode)
                  << ", has_albedo_texture="
                  << (!first_material.albedo_texture_path.empty() ? "true" : "false") << "}";
        return true;
    };

    const auto try_load_animated_asset = [&](const std::string& path,
                                             const char* source_label) -> bool {
        animated_gltf::AnimatedGltfLoadResult loaded = animated_gltf::load_from_file(path);
        if (!loaded.ok) {
            LOG(WARNING) << "ClientApp: animated glTF load failed for " << source_label << "='"
                         << path << "' error='" << loaded.error_message
                         << "'; falling back to static mesh path";
            return try_load_static_asset(path, source_label);
        }
        animated_asset_.emplace(std::move(loaded.asset));
        std::string playback_error;
        if (!animation_playback_.set_asset(&*animated_asset_, &playback_error)) {
            LOG(WARNING) << "ClientApp: animation playback setup failed for " << source_label
                         << "='" << path << "' error='" << playback_error
                         << "'; falling back to static mesh path";
            animated_asset_.reset();
            animation_playback_.clear_asset();
            return try_load_static_asset(path, source_label);
        }
        configure_animation_playback_from_environment();
        populate_world_from_animated_asset();

        LOG(INFO) << "ClientApp: loaded animated glTF from " << source_label << "='" << path
                  << "', clips=" << animated_asset_->clips.size()
                  << ", primitives=" << animated_asset_->primitives.size()
                  << ", playback_meshes=" << animated_mesh_bindings_.size();
        if (animated_mesh_bindings_.empty()) {
            LOG(WARNING) << "ClientApp: animated glTF loaded but produced zero playable meshes";
        }
        return true;
    };

    const char* animated_asset_path = std::getenv(kAnimatedGltfAssetEnvVar);
    if (animated_asset_path != nullptr && animated_asset_path[0] != '\0') {
        if (try_load_animated_asset(animated_asset_path, kAnimatedGltfAssetEnvVar)) {
            return;
        }
    }

    std::string resolved_mesh_asset_path;
    const char* mesh_asset_path = std::getenv(kMeshAssetEnvVar);
    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        const std::string default_path = resolve_default_startup_model_path();
        if (!default_path.empty()) {
            LOG(INFO) << "ClientApp: no " << kMeshAssetEnvVar << " set; using default model path '"
                      << default_path << "'";
            if (try_load_animated_asset(default_path, "default_model_path")) {
                return;
            }
            resolved_mesh_asset_path = default_path;
            mesh_asset_path = resolved_mesh_asset_path.c_str();
        } else {
            LOG(INFO) << "ClientApp: no ISLA_MESH_ASSET set and default model path '"
                      << kDefaultStartupModelPath << "' not found; leaving scene empty";
            return;
        }
    }

    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        LOG(INFO) << "ClientApp: no ISLA_MESH_ASSET set, leaving scene empty";
        return;
    }

    if (!try_load_static_asset(mesh_asset_path, kMeshAssetEnvVar)) {
        LOG(WARNING) << "ClientApp: mesh load failed for ISLA_MESH_ASSET='" << mesh_asset_path
                     << "'; leaving scene empty";
    }
}

void ClientApp::configure_animation_playback_from_environment() {
    if (!animated_asset_.has_value()) {
        return;
    }
    std::string playback_error;
    const char* clip_name = std::getenv(kAnimationClipEnvVar);
    const std::size_t clip_index = find_clip_index_by_name(*animated_asset_, clip_name);
    if (clip_index < animated_asset_->clips.size()) {
        if (!animation_playback_.set_clip_index(clip_index, &playback_error)) {
            LOG(WARNING) << "ClientApp: failed selecting clip index " << clip_index << " error='"
                         << playback_error << "'";
        } else {
            LOG(INFO) << "ClientApp: selected animation clip index=" << clip_index << " name='"
                      << animated_asset_->clips[clip_index].name << "'";
        }
    } else if (clip_name != nullptr && clip_name[0] != '\0') {
        LOG(WARNING) << "ClientApp: requested clip '" << clip_name
                     << "' not found; defaulting to clip index 0";
    }

    const char* playback_mode = std::getenv(kAnimationPlaybackModeEnvVar);
    if (playback_mode == nullptr) {
        return;
    }
    const std::string mode_value(playback_mode);
    if (mode_value == "clamp") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Clamp);
        LOG(INFO) << "ClientApp: animation playback mode set to clamp";
    } else if (mode_value == "loop") {
        animation_playback_.set_playback_mode(animated_gltf::ClipPlaybackMode::Loop);
        LOG(INFO) << "ClientApp: animation playback mode set to loop";
    } else {
        LOG(WARNING) << "ClientApp: unknown " << kAnimationPlaybackModeEnvVar << " value='"
                     << mode_value << "'; expected 'loop' or 'clamp'";
    }
}

void ClientApp::populate_world_from_animated_asset() {
    animated_mesh_bindings_.clear();
    animation_tick_count_ = 0U;
    world_.meshes().clear();
    world_.objects().clear();
    if (!animated_asset_.has_value()) {
        return;
    }
    for (std::size_t primitive_index = 0U; primitive_index < animated_asset_->primitives.size();
         ++primitive_index) {
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset_->primitives[primitive_index];

        if (gpu_skinning_authoritative_) {
            std::vector<GpuSkinningPartition> partitions;
            std::string partition_error;
            const std::vector<SkinnedMeshVertex> render_vertices =
                make_render_skinned_vertices(primitive);
            if (!build_gpu_skinning_partitions(render_vertices, primitive.indices,
                                               kMaxGpuSkinningJoints, partitions,
                                               &partition_error)) {
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "ClientApp: failed to partition skinned primitive " << primitive_index
                    << " (source_vertices=" << render_vertices.size()
                    << ", source_indices=" << primitive.indices.size() << ")"
                    << " for GPU palette budget " << kMaxGpuSkinningJoints
                    << "; skipping primitive. error='" << partition_error << "'";
                continue;
            }
            if (partitions.empty()) {
                continue;
            }
            if (partitions.size() > 1U) {
                std::string palette_sizes;
                palette_sizes.reserve(partitions.size() * 4U);
                for (std::size_t partition_index = 0U; partition_index < partitions.size();
                     ++partition_index) {
                    if (!palette_sizes.empty()) {
                        palette_sizes += ",";
                    }
                    palette_sizes +=
                        std::to_string(partitions[partition_index].global_joint_palette.size());
                }
                LOG(INFO) << "ClientApp: primitive " << primitive_index << " split into "
                          << partitions.size() << " GPU skinning partitions (palette_sizes=["
                          << palette_sizes << "])";
            }

            for (GpuSkinningPartition& partition : partitions) {
                AnimatedMeshBinding binding;
                binding.primitive_index = primitive_index;
                binding.gpu_palette_global_joints = std::move(partition.global_joint_palette);
                if (binding.gpu_palette_global_joints.empty()) {
                    LOG_EVERY_N_SEC(WARNING, 2.0)
                        << "ClientApp: GPU partition has empty remapped joint palette "
                           "(primitive_index="
                        << primitive_index << ")";
                }

                MeshData mesh;
                mesh.set_triangles(
                    make_triangles_from_skinned_geometry(partition.vertices, partition.indices));
                mesh.set_skinned_geometry(std::move(partition.vertices),
                                          std::move(partition.indices));
                if (animation_playback_.has_cached_pose()) {
                    mesh.set_skin_palette(
                        make_remapped_skin_palette(animation_playback_.cached_pose().skin_matrices,
                                                   binding.gpu_palette_global_joints));
                }
                if (mesh.triangles().empty()) {
                    continue;
                }
                world_.meshes().push_back(std::move(mesh));
                binding.mesh_id = world_.meshes().size() - 1U;
                world_.objects().push_back(RenderObject{
                    .mesh_id = binding.mesh_id,
                    .material_id = 0U,
                    .visible = true,
                });
                animated_mesh_bindings_.push_back(std::move(binding));
            }
            continue;
        }

        AnimatedMeshBinding binding;
        binding.primitive_index = primitive_index;
        std::vector<Triangle> initial_triangles =
            animated_mesh_skinning::make_initial_triangles_and_workspace(
                primitive, &binding.skinning_workspace);

        MeshData mesh;
        mesh.set_triangles(std::move(initial_triangles));
        mesh.clear_skinned_geometry();
        if (mesh.triangles().empty()) {
            continue;
        }
        world_.meshes().push_back(std::move(mesh));
        binding.mesh_id = world_.meshes().size() - 1U;
        world_.objects().push_back(RenderObject{
            .mesh_id = binding.mesh_id,
            .material_id = 0U,
            .visible = true,
        });
        animated_mesh_bindings_.push_back(std::move(binding));
    }
    LOG(INFO) << "ClientApp: animated mesh population complete, bindings="
              << animated_mesh_bindings_.size() << ", gpu_skinning_authoritative="
              << (gpu_skinning_authoritative_ ? "true" : "false");
}

void ClientApp::tick() {
    const std::uint64_t now_ns = sdl_runtime_.get_ticks_ns();
    float dt_seconds = 0.0F;
    if (now_ns < last_tick_ns_) {
        LOG(WARNING) << "ClientApp: non-monotonic tick clock observed (now=" << now_ns
                     << ", previous=" << last_tick_ns_ << "); clamping dt to 0";
    } else {
        dt_seconds =
            static_cast<float>(now_ns - last_tick_ns_) / static_cast<float>(kNanosecondsPerSecond);
    }
    if (!std::isfinite(dt_seconds) || dt_seconds < 0.0F) {
        LOG(WARNING) << "ClientApp: invalid frame dt computed (" << dt_seconds
                     << "); clamping to 0";
        dt_seconds = 0.0F;
    }
    last_tick_ns_ = now_ns;
    world_.set_sim_time_seconds(world_.sim_time_seconds() + dt_seconds);

    SDL_Event event;
    while (sdl_runtime_.poll_event(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            is_running_ = false;
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            event.type == SDL_EVENT_WINDOW_RESIZED) {
            int width = window_width_;
            int height = window_height_;
            if (sdl_runtime_.get_window_size_in_pixels(window_, &width, &height)) {
                window_width_ = width;
                window_height_ = height;
                model_renderer_.on_resize(
                    RenderSize{ .width = window_width_, .height = window_height_ });
            }
        }
    }

    tick_animation(dt_seconds);
}

void ClientApp::tick_animation(float dt_seconds) {
    if (!animated_asset_.has_value()) {
        return;
    }
    std::string playback_error;
    if (!animation_playback_.tick(dt_seconds, &playback_error)) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ClientApp: animation playback tick failed: " << playback_error;
        return;
    }
    if (!animation_playback_.has_cached_pose()) {
        return;
    }
    ++animation_tick_count_;
    const bool should_recompute_bounds =
        (animation_tick_count_ % kAnimatedBoundsRecomputeIntervalTicks) == 0U;
    std::size_t recomputed_bounds_mesh_count = 0U;
    const std::vector<Mat4>& skin_matrices = animation_playback_.cached_pose().skin_matrices;
    const auto& playback_state = animation_playback_.state();
    LOG_EVERY_N_SEC(INFO, 2.0)
        << "ClientApp: animation playback tick clip_index=" << playback_state.clip_index
        << ", local_time_seconds=" << playback_state.local_time_seconds << ", mode="
        << (playback_state.playback_mode == animated_gltf::ClipPlaybackMode::Clamp ? "clamp"
                                                                                   : "loop");
    if (gpu_skinning_authoritative_) {
        for (AnimatedMeshBinding& binding : animated_mesh_bindings_) {
            if (binding.mesh_id >= world_.meshes().size()) {
                continue;
            }
            MeshData& mesh = world_.meshes()[binding.mesh_id];
            if (binding.gpu_palette_global_joints.empty()) {
                mesh.set_skin_palette(skin_matrices);
                continue;
            }
            mesh.set_skin_palette(
                make_remapped_skin_palette(skin_matrices, binding.gpu_palette_global_joints));
        }
        return;
    }
    for (AnimatedMeshBinding& binding : animated_mesh_bindings_) {
        if (binding.mesh_id >= world_.meshes().size() ||
            binding.primitive_index >= animated_asset_->primitives.size()) {
            continue;
        }
        const animated_gltf::SkinnedPrimitive& primitive =
            animated_asset_->primitives[binding.primitive_index];
        MeshData& mesh = world_.meshes()[binding.mesh_id];
        mesh.edit_triangles_without_recompute_bounds([&](MeshData::TriangleList& triangles) {
            animated_mesh_skinning::skin_primitive_in_place(
                primitive, &skin_matrices, &binding.skinning_workspace, &triangles);
        });
        if (should_recompute_bounds) {
            mesh.recompute_bounds();
            ++recomputed_bounds_mesh_count;
        }
    }
    if (should_recompute_bounds && recomputed_bounds_mesh_count > 0U) {
        VLOG(1) << "ClientApp: deferred animated mesh bounds recomputed for "
                << recomputed_bounds_mesh_count
                << " mesh(es) at animation_tick_count=" << animation_tick_count_;
    }
}

void ClientApp::render() const {
    model_renderer_.render(world_);
}

void ClientApp::shutdown() {
    model_renderer_.shutdown();

    if (renderer_ != nullptr) {
        sdl_runtime_.destroy_renderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        sdl_runtime_.destroy_window(window_);
        window_ = nullptr;
    }

    sdl_runtime_.quit();
    is_running_ = false;
}

} // namespace isla::client
