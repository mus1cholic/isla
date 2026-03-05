#include "client_app_startup_loader.hpp"

#include "absl/log/log.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "client_app_geometry_utils.hpp"
#include "client_app_texture_remap.hpp"
#include "isla/engine/render/mesh_asset_loader.hpp"
#include "model_intake.hpp"

namespace isla::client {
namespace {

constexpr std::string_view kMeshAssetEnvVar = "ISLA_MESH_ASSET";
constexpr std::string_view kAnimatedGltfAssetEnvVar = "ISLA_ANIMATED_GLTF_ASSET";

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

bool try_load_static_asset(StartupLoaderContext& context, const std::string& path,
                           const char* source_label) {
    mesh_asset_loader::MeshAssetLoadResult loaded = mesh_asset_loader::load_from_file(path);
    if (!loaded.ok) {
        LOG(WARNING) << "ClientApp: static mesh load failed for " << source_label << "='" << path
                     << "' error='" << loaded.error_message << "'";
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
    const StaticTextureRemapApplicationResult texturemap_result =
        apply_static_texture_remap(path, primitive_chunks);
    for (const std::string& info : texturemap_result.infos) {
        LOG(INFO) << "ClientApp: " << info;
    }
    for (const std::string& warning : texturemap_result.warnings) {
        LOG(WARNING) << "ClientApp: " << warning;
    }
    if (texturemap_result.sidecar_load_failed) {
        LOG(ERROR)
            << "ClientApp: static mesh load aborted due to invalid texture remap sidecar for "
            << source_label << "='" << path << "'";
        return false;
    }

    context.world.materials().clear();
    context.world.meshes().clear();
    context.world.objects().clear();
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
                << " uses MASK-like alpha cutoff without albedo texture path; cutout appearance "
                   "may degrade";
        }
        const bool has_texture = !material.albedo_texture_path.empty();
        const std::string texture_path_for_log =
            has_texture ? material.albedo_texture_path : "<none>";
        context.world.materials().push_back(std::move(material));

        MeshData mesh;
        total_triangle_count += chunk.triangles.size();
        mesh.set_triangles(std::move(chunk.triangles));
        context.world.meshes().push_back(std::move(mesh));
        const std::size_t mesh_id = context.world.meshes().size() - 1U;
        const std::size_t material_id = context.world.materials().size() - 1U;
        context.world.objects().push_back(RenderObject{
            .mesh_id = mesh_id,
            .material_id = material_id,
            .visible = true,
        });
        const std::string source_mesh_index =
            chunk.has_source_identity ? std::to_string(chunk.source_mesh_index) : "n/a";
        const std::string source_primitive_index =
            chunk.has_source_identity ? std::to_string(chunk.source_primitive_index) : "n/a";
        const std::string source_material_name =
            chunk.source_material_name.empty() ? "<none>" : chunk.source_material_name;
        const bool from_texturemap =
            chunk_index < texturemap_result.applied_from_texturemap.size() &&
            texturemap_result.applied_from_texturemap[chunk_index];
        const char* texture_source =
            !has_texture ? "none" : (from_texturemap ? "texturemap" : "gltf");
        LOG(INFO) << "ClientApp: static material inventory slot material_id=" << material_id
                  << " source_mesh_index=" << source_mesh_index
                  << " source_primitive_index=" << source_primitive_index
                  << " source_material_name='" << source_material_name << "'"
                  << " texture_source=" << texture_source << " texture_path='"
                  << texture_path_for_log << "'";
    }
    if (context.world.meshes().empty()) {
        LOG(WARNING) << "ClientApp: static mesh load produced no renderable primitive chunks for "
                     << source_label << "='" << path << "'";
        return false;
    }
    const Transform aggregate_transform =
        make_visible_object_transform_for_meshes(context.world.meshes());
    VLOG(1) << "ClientApp: static aggregate transform applied to " << context.world.objects().size()
            << " object(s) position=[" << aggregate_transform.position.x << ","
            << aggregate_transform.position.y << "," << aggregate_transform.position.z
            << "] scale=[" << aggregate_transform.scale.x << "," << aggregate_transform.scale.y
            << "," << aggregate_transform.scale.z << "]";
    for (RenderObject& object : context.world.objects()) {
        object.transform = aggregate_transform;
    }

    const Material& first_material = context.world.materials().front();
    VLOG(1) << "ClientApp: loaded static mesh from " << source_label << "='" << path
            << "' triangles=" << total_triangle_count
            << " primitive_meshes=" << context.world.meshes().size()
            << " materials=" << context.world.materials().size() << " first_material={base_color=["
            << first_material.base_color.r << "," << first_material.base_color.g << ","
            << first_material.base_color.b << "], base_alpha=" << first_material.base_alpha
            << ", alpha_cutoff=" << first_material.alpha_cutoff
            << ", blend_mode=" << material_blend_mode_name(first_material.blend_mode)
            << ", cull_mode=" << material_cull_mode_name(first_material.cull_mode)
            << ", has_albedo_texture="
            << (!first_material.albedo_texture_path.empty() ? "true" : "false") << "}";
    return true;
}

bool try_load_animated_asset(StartupLoaderContext& context, const std::string& path,
                             const char* source_label) {
    animated_gltf::AnimatedGltfLoadResult loaded = animated_gltf::load_from_file(path);
    if (!loaded.ok) {
        LOG(WARNING) << "ClientApp: animated glTF load failed for " << source_label << "='" << path
                     << "' error='" << loaded.error_message
                     << "'; falling back to static mesh path";
        return try_load_static_asset(context, path, source_label);
    }
    context.animated_asset.emplace(std::move(loaded.asset));
    std::string playback_error;
    if (!context.animation_playback.set_asset(&*context.animated_asset, &playback_error)) {
        LOG(WARNING) << "ClientApp: animation playback setup failed for " << source_label << "='"
                     << path << "' error='" << playback_error
                     << "'; falling back to static mesh path";
        context.animated_asset.reset();
        context.animation_playback.clear_asset();
        return try_load_static_asset(context, path, source_label);
    }
    context.configure_animation_playback_from_environment();
    context.load_physics_sidecar_for_asset(path);
    context.populate_world_from_animated_asset();
    const auto& playback_state = context.animation_playback.state();
    const bool clip_index_valid = playback_state.clip_index < context.animated_asset->clips.size();
    const std::string_view selected_clip_name =
        clip_index_valid
            ? std::string_view(context.animated_asset->clips[playback_state.clip_index].name)
            : std::string_view("<invalid_clip_index>");
    const float selected_clip_duration =
        clip_index_valid ? context.animated_asset->clips[playback_state.clip_index].duration_seconds
                         : -1.0F;
    const animated_gltf::AnimatedNodeSummary node_summary =
        animated_gltf::summarize_animated_nodes(*context.animated_asset);
    LOG(INFO) << "ClientApp: animated startup summary clip='" << std::string(selected_clip_name)
              << "' duration_seconds=" << selected_clip_duration << " gpu_skinning_authoritative="
              << (context.gpu_skinning_authoritative ? "true" : "false")
              << " physics_sidecar_loaded="
              << (context.physics_sidecar.has_value() ? "true" : "false")
              << " explicit_node_hierarchy="
              << (!context.animated_asset->nodes.empty() ? "true" : "false")
              << " node_count=" << context.animated_asset->nodes.size()
              << " animated_non_joint_nodes=" << node_summary.animated_non_joint_nodes;
    VLOG(1) << "ClientApp: loaded animated glTF from " << source_label << "='" << path
            << "', clips=" << context.animated_asset->clips.size()
            << ", primitives=" << context.animated_asset->primitives.size()
            << ", nodes=" << context.animated_asset->nodes.size()
            << ", animated_nodes=" << node_summary.animated_nodes
            << ", animated_non_joint_nodes=" << node_summary.animated_non_joint_nodes
            << ", playback_meshes=" << context.animated_mesh_bindings.size();
    if (context.animated_mesh_bindings.empty()) {
        LOG(WARNING) << "ClientApp: animated glTF loaded but produced zero playable meshes";
    }
    return true;
}

} // namespace

void load_startup_mesh(StartupLoaderContext& context) {
    context.world.materials().clear();
    context.world.materials().push_back(Material{});
    context.world.meshes().clear();
    context.world.objects().clear();
    context.animated_asset.reset();
    context.physics_sidecar.reset();
    context.animation_playback.clear_asset();
    context.animated_mesh_bindings.clear();
    context.physics_collider_bindings.clear();
    context.physics_proxy_material_id.reset();
    context.animation_tick_count = 0U;

    const char* animated_asset_path = std::getenv(kAnimatedGltfAssetEnvVar.data());
    if (animated_asset_path != nullptr && animated_asset_path[0] != '\0') {
        if (try_load_animated_asset(context, animated_asset_path,
                                    kAnimatedGltfAssetEnvVar.data())) {
            return;
        }
    }

    std::string resolved_mesh_asset_path;
    const char* mesh_asset_path = std::getenv(kMeshAssetEnvVar.data());
    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        const model_intake::ResolveStartupAssetResult intake_result =
            model_intake::resolve_startup_asset_from_models();
        for (const std::string& info : intake_result.infos) {
            VLOG(1) << "ClientApp: model intake info: " << info;
        }
        for (const std::string& warning : intake_result.warnings) {
            LOG(WARNING) << "ClientApp: model intake warning: " << warning;
        }
        if (intake_result.has_asset) {
            const char* source_label = intake_result.source_label.empty()
                                           ? "models_intake"
                                           : intake_result.source_label.c_str();
            LOG(INFO) << "ClientApp: startup asset selected from models intake path='"
                      << intake_result.runtime_asset_path << "' source_label='" << source_label
                      << "' used_pmx_conversion="
                      << (intake_result.used_pmx_conversion ? "true" : "false")
                      << " pmx_conversion_cache_hit="
                      << (intake_result.pmx_conversion_cache_hit ? "true" : "false");
            if (try_load_animated_asset(context, intake_result.runtime_asset_path, source_label)) {
                LOG(INFO)
                    << "ClientApp: startup asset loaded successfully via animated/static startup "
                       "path from models intake";
                return;
            }
            resolved_mesh_asset_path = intake_result.runtime_asset_path;
            mesh_asset_path = resolved_mesh_asset_path.c_str();
        } else {
            VLOG(1)
                << "ClientApp: no " << kMeshAssetEnvVar
                << " set and models intake did not resolve a startup asset; leaving scene empty";
            return;
        }
    }

    if (mesh_asset_path == nullptr || mesh_asset_path[0] == '\0') {
        VLOG(1) << "ClientApp: no ISLA_MESH_ASSET set, leaving scene empty";
        return;
    }

    if (!try_load_static_asset(context, mesh_asset_path, kMeshAssetEnvVar.data())) {
        LOG(WARNING) << "ClientApp: mesh load failed for ISLA_MESH_ASSET='" << mesh_asset_path
                     << "'; leaving scene empty";
    }
}

} // namespace isla::client
