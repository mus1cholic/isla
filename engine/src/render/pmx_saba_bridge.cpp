#include "include/pmx_saba_bridge.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <Saba/Model/MMD/PMXFile.h>

namespace isla::client::pmx_native::internal {
namespace {

bool is_ascii_alpha(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

bool is_absolute_texture_path(std::string_view texture_path) {
    if (texture_path.empty()) {
        return false;
    }
    if (texture_path.size() >= 3 && is_ascii_alpha(texture_path[0]) && texture_path[1] == ':' &&
        (texture_path[2] == '/' || texture_path[2] == '\\')) {
        return true;
    }
    if (texture_path[0] == '/' || texture_path[0] == '\\') {
        return true;
    }
    return false;
}

bool has_parent_traversal(std::string_view texture_path) {
    const std::filesystem::path path(texture_path);
    for (const std::filesystem::path& component : path) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

void append_weight_type_count(const saba::PMXVertexWeight weight_type, ProbeSummary& summary) {
    switch (weight_type) {
    case saba::PMXVertexWeight::BDEF1:
        ++summary.bdef1_vertex_count;
        break;
    case saba::PMXVertexWeight::BDEF2:
        ++summary.bdef2_vertex_count;
        break;
    case saba::PMXVertexWeight::BDEF4:
        ++summary.bdef4_vertex_count;
        break;
    case saba::PMXVertexWeight::SDEF:
        ++summary.sdef_vertex_count;
        break;
    case saba::PMXVertexWeight::QDEF:
        ++summary.qdef_vertex_count;
        break;
    }
}

} // namespace

ProbeResult probe_with_saba(std::string_view asset_path) {
    ProbeResult result;
    const std::filesystem::path asset_filesystem_path(asset_path);
    const std::filesystem::path asset_directory = asset_filesystem_path.parent_path();
    result.infos.push_back("native PMX probe backend='saba' pinned_commit='" +
                           std::string(backend_version_pin()) + "' asset='" +
                           asset_filesystem_path.string() + "'");

    saba::PMXFile pmx;
    if (!saba::ReadPMXFile(&pmx, std::string(asset_path).c_str())) {
        result.error_message = "native PMX probe backend='saba' failed to read asset '" +
                               asset_filesystem_path.string() +
                               "'; inspect file validity and UTF/path encoding";
        return result;
    }

    result.ok = true;
    result.summary.model_name = pmx.m_info.m_modelName;
    result.summary.vertex_count = pmx.m_vertices.size();
    result.summary.face_count = pmx.m_faces.size();
    result.summary.material_count = pmx.m_materials.size();
    result.summary.texture_count = pmx.m_textures.size();
    result.summary.bone_count = pmx.m_bones.size();
    result.summary.morph_count = pmx.m_morphs.size();
    result.summary.rigidbody_count = pmx.m_rigidbodies.size();
    result.summary.joint_count = pmx.m_joints.size();
    result.summary.softbody_count = pmx.m_softbodies.size();

    for (const saba::PMXVertex& vertex : pmx.m_vertices) {
        append_weight_type_count(vertex.m_weightType, result.summary);
    }

    for (const saba::PMXBone& bone : pmx.m_bones) {
        if ((static_cast<std::uint16_t>(bone.m_boneFlag) &
             static_cast<std::uint16_t>(saba::PMXBoneFlags::IK)) != 0U) {
            ++result.summary.ik_bone_count;
        }
        if ((static_cast<std::uint16_t>(bone.m_boneFlag) &
             static_cast<std::uint16_t>(saba::PMXBoneFlags::AppendRotate)) != 0U ||
            (static_cast<std::uint16_t>(bone.m_boneFlag) &
             static_cast<std::uint16_t>(saba::PMXBoneFlags::AppendTranslate)) != 0U) {
            ++result.summary.append_transform_bone_count;
        }
    }

    for (const saba::PMXTexture& texture : pmx.m_textures) {
        const std::filesystem::path texture_path(texture.m_textureName);
        if (is_absolute_texture_path(texture.m_textureName)) {
            ++result.summary.absolute_texture_reference_count;
        }
        if (has_parent_traversal(texture.m_textureName)) {
            ++result.summary.parent_traversal_texture_reference_count;
        }
        if (!texture.m_textureName.empty() && !is_absolute_texture_path(texture.m_textureName) &&
            !has_parent_traversal(texture.m_textureName)) {
            const std::filesystem::path resolved_texture_path =
                (asset_directory / texture_path).lexically_normal();
            std::error_code exists_error;
            if (!std::filesystem::exists(resolved_texture_path, exists_error) || exists_error) {
                ++result.summary.missing_texture_reference_count;
            }
        }
    }

    for (const saba::PMXMaterial& material : pmx.m_materials) {
        if (material.m_sphereTextureIndex >= 0) {
            ++result.summary.sphere_texture_material_count;
        }
        if (material.m_toonTextureIndex >= 0) {
            ++result.summary.toon_texture_material_count;
        }
        if ((static_cast<std::uint8_t>(material.m_drawMode) &
             static_cast<std::uint8_t>(saba::PMXDrawModeFlags::DrawEdge)) != 0U) {
            ++result.summary.edge_enabled_material_count;
        }
    }

    if (result.summary.sdef_vertex_count > 0U) {
        result.warnings.push_back("PMX probe detected SDEF vertices count=" +
                                  std::to_string(result.summary.sdef_vertex_count) +
                                  "; Phase 0 marks SDEF as an explicit fallback requirement "
                                  "rather than a supported direct runtime skinning path");
    }
    if (result.summary.qdef_vertex_count > 0U) {
        result.warnings.push_back("PMX probe detected QDEF vertices count=" +
                                  std::to_string(result.summary.qdef_vertex_count) +
                                  "; Phase 0 marks QDEF as an explicit fallback requirement "
                                  "rather than a supported direct runtime skinning path");
    }
    if (result.summary.morph_count > 0U) {
        result.warnings.push_back(
            "PMX probe detected morph data count=" + std::to_string(result.summary.morph_count) +
            "; morph runtime parity is deferred beyond the initial native "
            "PMX baseline");
    }
    if (result.summary.rigidbody_count > 0U || result.summary.joint_count > 0U ||
        result.summary.softbody_count > 0U) {
        result.warnings.push_back("PMX probe detected physics-related data rigidbodies=" +
                                  std::to_string(result.summary.rigidbody_count) +
                                  " joints=" + std::to_string(result.summary.joint_count) +
                                  " softbodies=" + std::to_string(result.summary.softbody_count) +
                                  "; native PMX physics is deferred beyond the initial runtime "
                                  "baseline");
    }
    if (result.summary.sphere_texture_material_count > 0U ||
        result.summary.toon_texture_material_count > 0U ||
        result.summary.edge_enabled_material_count > 0U) {
        result.warnings.push_back(
            "PMX probe detected toon/sphere/edge material channels "
            "sphere_materials=" +
            std::to_string(result.summary.sphere_texture_material_count) +
            " toon_materials=" + std::to_string(result.summary.toon_texture_material_count) +
            " edge_materials=" + std::to_string(result.summary.edge_enabled_material_count) +
            "; Phase 0 keeps those channels explicitly deferred instead of "
            "silently approximating them");
    }
    if (result.summary.absolute_texture_reference_count > 0U ||
        result.summary.parent_traversal_texture_reference_count > 0U) {
        result.warnings.push_back(
            "PMX probe detected texture paths that violate the planned asset-relative-only "
            "ingestion policy absolute_texture_refs=" +
            std::to_string(result.summary.absolute_texture_reference_count) +
            " parent_traversal_texture_refs=" +
            std::to_string(result.summary.parent_traversal_texture_reference_count));
    }
    if (result.summary.missing_texture_reference_count > 0U) {
        result.warnings.push_back("PMX probe detected missing asset-relative textures count=" +
                                  std::to_string(result.summary.missing_texture_reference_count) +
                                  "; resolve missing texture files before native PMX material "
                                  "hookup");
    }

    result.infos.push_back("PMX probe succeeded asset='" + asset_filesystem_path.string() +
                           "' vertices=" + std::to_string(result.summary.vertex_count) +
                           " faces=" + std::to_string(result.summary.face_count) +
                           " materials=" + std::to_string(result.summary.material_count) +
                           " bones=" + std::to_string(result.summary.bone_count) +
                           " warnings=" + std::to_string(result.warnings.size()) +
                           " skinning={BDEF1:" + std::to_string(result.summary.bdef1_vertex_count) +
                           ",BDEF2:" + std::to_string(result.summary.bdef2_vertex_count) +
                           ",BDEF4:" + std::to_string(result.summary.bdef4_vertex_count) +
                           ",SDEF:" + std::to_string(result.summary.sdef_vertex_count) +
                           ",QDEF:" + std::to_string(result.summary.qdef_vertex_count) + "}");
    return result;
}

} // namespace isla::client::pmx_native::internal
