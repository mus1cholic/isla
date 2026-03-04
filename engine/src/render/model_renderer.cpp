#include "isla/engine/render/model_renderer.hpp"

#include "absl/log/log.h"
#include <SDL3/SDL.h>

#if defined(_WIN32)

#include "engine/src/render/include/bgfx_mesh_manager.hpp"
#include "engine/src/render/include/bgfx_shader_manager.hpp"
#include "engine/src/render/include/bgfx_texture_manager.hpp"
#include "engine/src/render/include/model_renderer_skinning_utils.hpp"
#include "isla/engine/math/mat4.hpp"
#include "isla/engine/render/overlay_transparency.hpp"
#include "isla/engine/render/render_world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

namespace isla::client {

namespace {

constexpr std::uint32_t kBgfxResetFlags = 0;
constexpr std::uint8_t kBgfxViewId = 0;
constexpr float kDefaultCameraDistance = 3.0F;
constexpr float kDefaultCameraFovYDegrees = 60.0F;
constexpr float kDefaultCameraNear = 0.05F;
constexpr float kDefaultCameraFar = 1000.0F;
constexpr const char* kObjectColorUniformName = "u_object_color";
constexpr const char* kDirLightDirUniformName = "u_dir_light_dir";
constexpr const char* kDirLightColorUniformName = "u_dir_light_color";
constexpr const char* kAmbientColorUniformName = "u_ambient_color";
constexpr const char* kCameraPosUniformName = "u_camera_pos";
constexpr const char* kSpecParamsUniformName = "u_spec_params";
constexpr const char* kAlphaParamsUniformName = "u_alpha_params";
constexpr const char* kJointPaletteUniformName = "u_joint_palette";
constexpr std::uint64_t kOpaqueRenderStateBase = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                                 BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                                                 BGFX_STATE_MSAA;
constexpr std::uint64_t kAlphaBlendRenderStateBase = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                                     BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA |
                                                     BGFX_STATE_BLEND_ALPHA;

bool vec3_is_finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::uint64_t cull_state_for_material(MaterialCullMode mode) {
    switch (mode) {
    case MaterialCullMode::Clockwise:
        return BGFX_STATE_CULL_CW;
    case MaterialCullMode::CounterClockwise:
        return BGFX_STATE_CULL_CCW;
    case MaterialCullMode::Disabled:
        return 0ULL;
    }
    return BGFX_STATE_CULL_CW;
}

} // namespace

class ModelRenderer::Impl {
  public:
    int window_width = kDefaultRenderWidth;
    int window_height = kDefaultRenderHeight;
    SDL_Window* window = nullptr;
    bool initialized = false;
    BgfxMeshManager mesh_manager;
    BgfxShaderManager shader_manager;
    BgfxTextureManager texture_manager;
    bgfx::UniformHandle time_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle object_color_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle dir_light_dir_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle dir_light_color_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle ambient_color_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle camera_pos_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle spec_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle alpha_params_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle joint_palette_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle tex_color_uniform = BGFX_INVALID_HANDLE;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    std::vector<Mat4> joint_palette_upload;
};

namespace {

void destroy_uniform_if_valid(bgfx::UniformHandle& handle) {
    if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
        handle = BGFX_INVALID_HANDLE;
    }
}

void destroy_renderer_uniforms(ModelRenderer::Impl& impl) {
    destroy_uniform_if_valid(impl.time_uniform);
    destroy_uniform_if_valid(impl.object_color_uniform);
    destroy_uniform_if_valid(impl.dir_light_dir_uniform);
    destroy_uniform_if_valid(impl.dir_light_color_uniform);
    destroy_uniform_if_valid(impl.ambient_color_uniform);
    destroy_uniform_if_valid(impl.camera_pos_uniform);
    destroy_uniform_if_valid(impl.spec_params_uniform);
    destroy_uniform_if_valid(impl.alpha_params_uniform);
    destroy_uniform_if_valid(impl.joint_palette_uniform);
    destroy_uniform_if_valid(impl.tex_color_uniform);
}

} // namespace

ModelRenderer::ModelRenderer() : impl_(std::make_unique<Impl>()) {}
ModelRenderer::~ModelRenderer() = default;

bool ModelRenderer::initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) {
    (void)renderer;
    impl_->window = window;
    impl_->window_width = std::max(1, size.width);
    impl_->window_height = std::max(1, size.height);

    if (impl_->window == nullptr) {
        LOG(ERROR) << "ModelRenderer: initialize failed: null window";
        return false;
    }

    void* native_window_handle = nullptr;
    const SDL_PropertiesID window_properties = SDL_GetWindowProperties(impl_->window);
    native_window_handle =
        SDL_GetPointerProperty(window_properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (native_window_handle == nullptr) {
        LOG(ERROR) << "ModelRenderer: initialize failed: could not obtain native window handle";
        return false;
    }

    bgfx::PlatformData platform_data{};
    platform_data.nwh = native_window_handle;

    bgfx::Init init{};
    init.type = bgfx::RendererType::Count;
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData = platform_data;
    init.resolution.width = static_cast<std::uint32_t>(impl_->window_width);
    init.resolution.height = static_cast<std::uint32_t>(impl_->window_height);
    init.resolution.reset = kBgfxResetFlags;
    if (!bgfx::init(init)) {
        LOG(ERROR) << "ModelRenderer: initialize failed: bgfx::init returned false";
        return false;
    }

    const auto clear_color =
        (static_cast<std::uint32_t>(OverlayTransparencyConfig::kColorKeyRed) << 24U) |
        (static_cast<std::uint32_t>(OverlayTransparencyConfig::kColorKeyGreen) << 16U) |
        (static_cast<std::uint32_t>(OverlayTransparencyConfig::kColorKeyBlue) << 8U) | 0xFFU;
    bgfx::setViewClear(kBgfxViewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clear_color, 1.0F, 0);
    bgfx::setViewRect(kBgfxViewId, 0, 0, static_cast<std::uint16_t>(impl_->window_width),
                      static_cast<std::uint16_t>(impl_->window_height));

    impl_->time_uniform = bgfx::createUniform("u_time", bgfx::UniformType::Vec4);
    impl_->object_color_uniform =
        bgfx::createUniform(kObjectColorUniformName, bgfx::UniformType::Vec4);
    impl_->dir_light_dir_uniform =
        bgfx::createUniform(kDirLightDirUniformName, bgfx::UniformType::Vec4);
    impl_->dir_light_color_uniform =
        bgfx::createUniform(kDirLightColorUniformName, bgfx::UniformType::Vec4);
    impl_->ambient_color_uniform =
        bgfx::createUniform(kAmbientColorUniformName, bgfx::UniformType::Vec4);
    impl_->camera_pos_uniform = bgfx::createUniform(kCameraPosUniformName, bgfx::UniformType::Vec4);
    impl_->spec_params_uniform =
        bgfx::createUniform(kSpecParamsUniformName, bgfx::UniformType::Vec4);
    impl_->alpha_params_uniform =
        bgfx::createUniform(kAlphaParamsUniformName, bgfx::UniformType::Vec4);
    impl_->joint_palette_uniform = bgfx::createUniform(
        kJointPaletteUniformName, bgfx::UniformType::Mat4, kMaxGpuSkinningJoints);
    impl_->tex_color_uniform = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(impl_->time_uniform) || !bgfx::isValid(impl_->object_color_uniform) ||
        !bgfx::isValid(impl_->dir_light_dir_uniform) ||
        !bgfx::isValid(impl_->dir_light_color_uniform) ||
        !bgfx::isValid(impl_->ambient_color_uniform) || !bgfx::isValid(impl_->camera_pos_uniform) ||
        !bgfx::isValid(impl_->spec_params_uniform) || !bgfx::isValid(impl_->alpha_params_uniform) ||
        !bgfx::isValid(impl_->joint_palette_uniform) || !bgfx::isValid(impl_->tex_color_uniform)) {
        LOG(ERROR) << "ModelRenderer: initialize failed: could not create renderer uniforms";
        destroy_renderer_uniforms(*impl_);
        bgfx::shutdown();
        return false;
    }

    if (!impl_->shader_manager.initialize() || !impl_->texture_manager.initialize() ||
        !impl_->mesh_manager.initialize()) {
        impl_->mesh_manager.shutdown();
        impl_->texture_manager.shutdown();
        impl_->shader_manager.shutdown();
        destroy_renderer_uniforms(*impl_);
        bgfx::shutdown();
        LOG(ERROR) << "ModelRenderer: initialize failed: renderer managers could not initialize";
        return false;
    }

    impl_->start_time = std::chrono::steady_clock::now();
    impl_->joint_palette_upload.assign(kMaxGpuSkinningJoints, Mat4::identity());
    impl_->initialized = true;
    return true;
}

bool ModelRenderer::uses_sdl_renderer() const {
    return false;
}

bool ModelRenderer::has_homogeneous_depth() const {
    if (!impl_ || !impl_->initialized) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    return (caps != nullptr) ? caps->homogeneousDepth : false;
}

bool ModelRenderer::supports_gpu_skinning() const {
    if (!impl_ || !impl_->initialized) {
        return false;
    }
    if (!bgfx::isValid(impl_->joint_palette_uniform)) {
        return false;
    }
    return bgfx::isValid(impl_->shader_manager.resolve_skinned_program("mesh"));
}

void ModelRenderer::on_resize(RenderSize size) {
    impl_->window_width = std::max(1, size.width);
    impl_->window_height = std::max(1, size.height);
    if (!impl_->initialized) {
        return;
    }

    bgfx::reset(static_cast<std::uint32_t>(impl_->window_width),
                static_cast<std::uint32_t>(impl_->window_height), kBgfxResetFlags);
    bgfx::setViewRect(kBgfxViewId, 0, 0, static_cast<std::uint16_t>(impl_->window_width),
                      static_cast<std::uint16_t>(impl_->window_height));
}

void ModelRenderer::render(const RenderWorld& world) const {
    if (!impl_->initialized) {
        return;
    }
    impl_->mesh_manager.begin_frame();
    impl_->mesh_manager.upload_dirty_meshes(world);

    const auto elapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - impl_->start_time).count();
    const float sim_time =
        std::isfinite(world.sim_time_seconds()) ? std::max(0.0F, world.sim_time_seconds()) : 0.0F;
    const std::array<float, 4> time_values{ sim_time, elapsed, 0.0F, 0.0F };
    bgfx::setUniform(impl_->time_uniform, time_values.data());

    Vec3 light_dir = world.directional_light().direction;
    if (!vec3_is_finite(light_dir)) {
        light_dir = kDefaultDirectionalLightDirection;
    }

    const Vec3 camera_eye{ .x = 0.0F, .y = 0.0F, .z = -kDefaultCameraDistance };
    const std::array<float, 4> dir_light_dir_values{ light_dir.x, light_dir.y, light_dir.z, 0.0F };
    const std::array<float, 4> dir_light_color_values{
        world.directional_light().color.r,
        world.directional_light().color.g,
        world.directional_light().color.b,
        1.0F,
    };
    const std::array<float, 4> ambient_color_values{
        world.ambient_light().color.r,
        world.ambient_light().color.g,
        world.ambient_light().color.b,
        1.0F,
    };
    const std::array<float, 4> camera_pos_values{ camera_eye.x, camera_eye.y, camera_eye.z, 1.0F };
    const std::array<float, 4> spec_params_values{ 0.85F, 16.0F, 0.0F, 0.0F };
    bgfx::setUniform(impl_->dir_light_dir_uniform, dir_light_dir_values.data());
    bgfx::setUniform(impl_->dir_light_color_uniform, dir_light_color_values.data());
    bgfx::setUniform(impl_->ambient_color_uniform, ambient_color_values.data());
    bgfx::setUniform(impl_->camera_pos_uniform, camera_pos_values.data());
    bgfx::setUniform(impl_->spec_params_uniform, spec_params_values.data());

    const Mat4 view = Mat4::look_at(LookAtParams{
        .eye = camera_eye,
        .target = Vec3{},
        .up = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
    });
    const float aspect =
        static_cast<float>(impl_->window_width) / static_cast<float>(impl_->window_height);
    const Mat4 proj = Mat4::perspective(kDefaultCameraFovYDegrees, aspect, kDefaultCameraNear,
                                        kDefaultCameraFar, has_homogeneous_depth());

    bgfx::setViewRect(kBgfxViewId, 0, 0, static_cast<std::uint16_t>(impl_->window_width),
                      static_cast<std::uint16_t>(impl_->window_height));
    bgfx::setViewTransform(kBgfxViewId, view.data(), proj.data());
    bgfx::touch(kBgfxViewId);

    const bool renderer_supports_gpu_skinning = supports_gpu_skinning();
    static const Material kFallbackMaterial;
    std::size_t submitted_draws = 0U;
    std::size_t opaque_draws = 0U;
    std::size_t alpha_blend_draws = 0U;
    std::size_t alpha_cutout_draws = 0U;
    for (const RenderObject& object : world.objects()) {
        if (!object.visible || !impl_->mesh_manager.has_mesh_slot(object.mesh_id)) {
            continue;
        }
        const MeshData& mesh = world.meshes().at(object.mesh_id);
        const bgfx::VertexBufferHandle vertex_buffer =
            impl_->mesh_manager.vertex_buffer_for_mesh(object.mesh_id);
        const bgfx::IndexBufferHandle index_buffer =
            impl_->mesh_manager.index_buffer_for_mesh(object.mesh_id);
        if (!bgfx::isValid(vertex_buffer) || !bgfx::isValid(index_buffer)) {
            continue;
        }

        const Material& material = [&]() -> const Material& {
            if (object.material_id < world.materials().size()) {
                return world.materials().at(object.material_id);
            }
            return kFallbackMaterial;
        }();
        const bool mesh_is_skinned = impl_->mesh_manager.mesh_is_skinned(object.mesh_id);
        const bool has_skin_palette = !mesh.skin_palette().empty();
        bgfx::ProgramHandle skinned_program = BGFX_INVALID_HANDLE;
        bool skinned_program_valid = false;
        if (mesh_is_skinned && has_skin_palette && renderer_supports_gpu_skinning) {
            skinned_program = impl_->shader_manager.resolve_skinned_program(material.shader_name);
            skinned_program_valid = bgfx::isValid(skinned_program);
            if (!skinned_program_valid) {
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "ModelRenderer: skinned program unavailable for shader='"
                    << material.shader_name
                    << "', falling back to static mesh program for skinned draw";
            }
        }
        const SkinningProgramPath program_path =
            choose_skinning_program_path(SkinningProgramDecisionInputs{
                .mesh_is_skinned = mesh_is_skinned,
                .has_skin_palette = has_skin_palette,
                .gpu_skinning_supported = renderer_supports_gpu_skinning,
                .skinned_program_valid = skinned_program_valid,
            });
        const bool use_skinned_program = program_path == SkinningProgramPath::SkinnedMesh;
        bgfx::ProgramHandle program =
            use_skinned_program ? skinned_program
                                : impl_->shader_manager.resolve_program(material.shader_name);
        if (!bgfx::isValid(program)) {
            continue;
        }
        if (use_skinned_program) {
            const bool truncated =
                fill_skin_palette_upload_buffer(mesh.skin_palette(), impl_->joint_palette_upload);
            if (truncated) {
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "ModelRenderer: skin palette size " << mesh.skin_palette().size()
                    << " exceeds GPU uniform limit " << kMaxGpuSkinningJoints
                    << "; truncating palette";
            }
            bgfx::setUniform(impl_->joint_palette_uniform, impl_->joint_palette_upload.data(),
                             kMaxGpuSkinningJoints);
        }
        const bgfx::TextureHandle texture =
            impl_->texture_manager.resolve_texture(material.albedo_texture_path);
        const MaterialRenderPathDecision material_path =
            choose_material_render_path(MaterialRenderPathDecisionInputs{
                .blend_mode = material.blend_mode,
                .base_alpha = material.base_alpha,
                .alpha_cutoff = material.alpha_cutoff,
            });
        const std::array<float, 4> object_color{
            material.base_color.r,
            material.base_color.g,
            material.base_color.b,
            material_path.alpha,
        };
        const std::array<float, 4> alpha_params{
            material_path.alpha_cutoff,
            material_path.alpha_cutout_enabled ? 1.0F : 0.0F,
            0.0F,
            0.0F,
        };
        if (material_path.alpha_cutout_enabled) {
            VLOG(1) << "ModelRenderer: alpha-cutout active for mesh_id=" << object.mesh_id
                    << " material_id=" << object.material_id
                    << " cutoff=" << material_path.alpha_cutoff;
        }
        const Mat4 model = Mat4::from_position_scale_quat(
            object.transform.position, object.transform.scale, object.transform.rotation);
        const std::uint64_t render_state_base = material_path.use_alpha_blend_base
                                                    ? kAlphaBlendRenderStateBase
                                                    : kOpaqueRenderStateBase;
        if (material_path.alpha_cutout_enabled) {
            ++alpha_cutout_draws;
        } else if (material_path.use_alpha_blend_base) {
            ++alpha_blend_draws;
        } else {
            ++opaque_draws;
        }
        const std::uint64_t render_state =
            render_state_base | cull_state_for_material(material.cull_mode);
        bgfx::setUniform(impl_->object_color_uniform, object_color.data());
        bgfx::setUniform(impl_->alpha_params_uniform, alpha_params.data());
        bgfx::setTexture(0, impl_->tex_color_uniform, texture);
        bgfx::setTransform(model.data());
        bgfx::setVertexBuffer(0, vertex_buffer);
        bgfx::setIndexBuffer(index_buffer);
        bgfx::setState(render_state);
        bgfx::submit(kBgfxViewId, program);
        ++submitted_draws;
    }
    VLOG(1) << "ModelRenderer: frame draw summary submitted_draws=" << submitted_draws
            << " opaque=" << opaque_draws << " alpha_blend=" << alpha_blend_draws
            << " alpha_cutout=" << alpha_cutout_draws << " world_objects=" << world.objects().size()
            << " world_meshes=" << world.meshes().size();
    if (!world.objects().empty() && submitted_draws == 0U) {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "ModelRenderer: world has render objects but no draws were submitted; check mesh "
               "uploads, material program resolution, or visibility flags";
    }

    bgfx::frame();
}

void ModelRenderer::set_debug_overlay_enabled(bool enabled) {
    (void)enabled;
}

void ModelRenderer::set_debug_overlay_lines(std::span<const std::string> lines) {
    (void)lines;
}

void ModelRenderer::shutdown() {
    if (!impl_->initialized) {
        return;
    }
    impl_->mesh_manager.shutdown();
    impl_->texture_manager.shutdown();
    impl_->shader_manager.shutdown();
    destroy_renderer_uniforms(*impl_);
    bgfx::shutdown();
    impl_->initialized = false;
    impl_->window = nullptr;
}

} // namespace isla::client

#else

namespace isla::client {

class ModelRenderer::Impl {};

ModelRenderer::ModelRenderer() : impl_(std::make_unique<Impl>()) {}
ModelRenderer::~ModelRenderer() = default;

bool ModelRenderer::initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) {
    (void)window;
    (void)renderer;
    (void)size;
    LOG(ERROR) << "ModelRenderer: bgfx renderer currently supports Windows only";
    return false;
}

bool ModelRenderer::uses_sdl_renderer() const {
    return false;
}

bool ModelRenderer::has_homogeneous_depth() const {
    return false;
}

bool ModelRenderer::supports_gpu_skinning() const {
    return false;
}

void ModelRenderer::on_resize(RenderSize size) {
    (void)size;
}

void ModelRenderer::render(const RenderWorld& world) const {
    (void)world;
}

void ModelRenderer::set_debug_overlay_enabled(bool enabled) {
    (void)enabled;
}

void ModelRenderer::set_debug_overlay_lines(std::span<const std::string> lines) {
    (void)lines;
}

void ModelRenderer::shutdown() {}

} // namespace isla::client

#endif
