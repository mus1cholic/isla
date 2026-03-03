#include "engine/src/render/include/bgfx_shader_manager.hpp"

#if defined(_WIN32)

#include "engine/src/render/include/shader_path_resolver.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/log/log.h"
#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>

namespace isla::client {

class BgfxShaderManager::Impl {
  public:
    std::unordered_map<std::string, bgfx::ProgramHandle> program_cache;
    std::unordered_map<std::string, bgfx::ProgramHandle> instanced_program_cache;
    std::unordered_map<std::string, bgfx::ProgramHandle> skinned_program_cache;
};

namespace {

bgfx::ShaderHandle load_shader_from_path(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return BGFX_INVALID_HANDLE;
    }

    const std::ifstream::pos_type length = stream.tellg();
    if (length <= 0) {
        return BGFX_INVALID_HANDLE;
    }
    stream.seekg(0, std::ios::beg);

    std::vector<char> data(static_cast<std::size_t>(length));
    if (!stream.read(data.data(), static_cast<std::streamsize>(length))) {
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* memory = bgfx::copy(data.data(), static_cast<std::uint32_t>(data.size()));
    return bgfx::createShader(memory);
}

bgfx::ShaderHandle load_shader_with_fallbacks(const char* shader_file_name) {
    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* runfiles_manifest_file = std::getenv("RUNFILES_MANIFEST_FILE");
    const char* base_path = SDL_GetBasePath();
    const std::string executable_base_path = base_path == nullptr ? "" : base_path;
    const ShaderPathLookup lookup{
        .shader_file_name = shader_file_name,
        .executable_base_path = executable_base_path,
        .runfiles_dir = runfiles_dir == nullptr ? "" : runfiles_dir,
        .test_srcdir = test_srcdir == nullptr ? "" : test_srcdir,
        .runfiles_manifest_file = runfiles_manifest_file == nullptr ? "" : runfiles_manifest_file,
    };
    VLOG(1) << "BgfxShaderManager: resolving shader '" << shader_file_name << "' with base_path='"
            << executable_base_path << "', RUNFILES_DIR='"
            << (runfiles_dir == nullptr ? "" : runfiles_dir) << "', TEST_SRCDIR='"
            << (test_srcdir == nullptr ? "" : test_srcdir) << "', RUNFILES_MANIFEST_FILE='"
            << (runfiles_manifest_file == nullptr ? "" : runfiles_manifest_file) << "'";

    const std::vector<std::string> candidates = build_shader_path_candidates(lookup);
    VLOG(1) << "BgfxShaderManager: shader '" << shader_file_name
            << "' candidate count=" << candidates.size();

    const std::string resolved_path = find_existing_shader_path(candidates);
    if (!resolved_path.empty()) {
        bgfx::ShaderHandle handle = load_shader_from_path(resolved_path);
        if (bgfx::isValid(handle)) {
            LOG(INFO) << "BgfxShaderManager: loaded shader '" << shader_file_name << "' from '"
                      << resolved_path << "'";
            return handle;
        }
        LOG(WARNING) << "BgfxShaderManager: resolved shader path '" << resolved_path
                     << "' but bgfx shader creation failed for '" << shader_file_name << "'";
    }

    LOG(ERROR) << "BgfxShaderManager: shader load failed for '" << shader_file_name
               << "'. Tried paths:";
    for (const std::string& path : candidates) {
        LOG(ERROR) << "  - " << path;
    }
    return BGFX_INVALID_HANDLE;
}

void destroy_shader_if_valid(bgfx::ShaderHandle handle) {
    if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
    }
}

} // namespace

BgfxShaderManager::BgfxShaderManager() : impl_(std::make_unique<Impl>()) {}

BgfxShaderManager::~BgfxShaderManager() = default;

bool BgfxShaderManager::initialize() {
    if (!impl_->program_cache.empty()) {
        shutdown();
    }

    bgfx::ShaderHandle vertex_shader = load_shader_with_fallbacks("vs_mesh.bin");
    bgfx::ShaderHandle fragment_shader = load_shader_with_fallbacks("fs_mesh.bin");
    if (!bgfx::isValid(vertex_shader) || !bgfx::isValid(fragment_shader)) {
        LOG(ERROR) << "BgfxShaderManager: initialize failed: could not load shader binaries";
        destroy_shader_if_valid(vertex_shader);
        destroy_shader_if_valid(fragment_shader);
        return false;
    }

    bgfx::ProgramHandle mesh_program = bgfx::createProgram(vertex_shader, fragment_shader, true);
    if (!bgfx::isValid(mesh_program)) {
        LOG(ERROR)
            << "BgfxShaderManager: initialize failed: could not create default mesh bgfx program";
        destroy_shader_if_valid(vertex_shader);
        destroy_shader_if_valid(fragment_shader);
        return false;
    }

    impl_->program_cache.emplace("mesh", mesh_program);

    bgfx::ShaderHandle skinned_vertex_shader = load_shader_with_fallbacks("vs_mesh_skinned.bin");
    if (!bgfx::isValid(skinned_vertex_shader)) {
        LOG(WARNING) << "BgfxShaderManager: could not load skinned vertex shader; "
                        "GPU skinning path will be unavailable";
    } else {
        // Fragment shader is shared; load a second copy because bgfx::createProgram with
        // destroy_shaders=true takes ownership.
        bgfx::ShaderHandle skinned_fragment_shader = load_shader_with_fallbacks("fs_mesh.bin");
        if (!bgfx::isValid(skinned_fragment_shader)) {
            LOG(WARNING) << "BgfxShaderManager: could not load fragment shader for skinned "
                            "program; GPU skinning path will be unavailable";
            destroy_shader_if_valid(skinned_vertex_shader);
        } else {
            bgfx::ProgramHandle skinned_program =
                bgfx::createProgram(skinned_vertex_shader, skinned_fragment_shader, true);
            if (!bgfx::isValid(skinned_program)) {
                LOG(WARNING) << "BgfxShaderManager: could not create skinned bgfx program; "
                                "GPU skinning path will be unavailable";
            } else {
                impl_->skinned_program_cache.emplace("mesh", skinned_program);
                LOG(INFO) << "BgfxShaderManager: skinned mesh program created successfully";
            }
        }
    }

    bgfx::ShaderHandle instanced_vertex_shader =
        load_shader_with_fallbacks("vs_mesh_instanced.bin");
    if (!bgfx::isValid(instanced_vertex_shader)) {
        LOG(WARNING) << "BgfxShaderManager: could not load instanced vertex shader; "
                        "instanced rendering path will be unavailable";
        return true;
    }

    // Fragment shader is shared 窶・load a second copy since bgfx::createProgram
    // with destroy_shaders=true takes ownership.
    bgfx::ShaderHandle instanced_fragment_shader = load_shader_with_fallbacks("fs_mesh.bin");
    if (!bgfx::isValid(instanced_fragment_shader)) {
        LOG(WARNING) << "BgfxShaderManager: could not load fragment shader for instanced program; "
                        "instanced rendering path will be unavailable";
        destroy_shader_if_valid(instanced_vertex_shader);
        return true;
    }

    bgfx::ProgramHandle instanced_program =
        bgfx::createProgram(instanced_vertex_shader, instanced_fragment_shader, true);
    if (!bgfx::isValid(instanced_program)) {
        LOG(WARNING) << "BgfxShaderManager: could not create instanced bgfx program; "
                        "instanced rendering path will be unavailable";
        return true;
    }

    impl_->instanced_program_cache.emplace("mesh", instanced_program);
    LOG(INFO) << "BgfxShaderManager: instanced mesh program created successfully";
    return true;
}

void BgfxShaderManager::shutdown() {
    for (auto& [name, program] : impl_->program_cache) {
        if (bgfx::isValid(program)) {
            bgfx::destroy(program);
        }
    }
    impl_->program_cache.clear();
    for (auto& [name, program] : impl_->instanced_program_cache) {
        if (bgfx::isValid(program)) {
            bgfx::destroy(program);
        }
    }
    impl_->instanced_program_cache.clear();
    for (auto& [name, program] : impl_->skinned_program_cache) {
        if (bgfx::isValid(program)) {
            bgfx::destroy(program);
        }
    }
    impl_->skinned_program_cache.clear();
}

bgfx::ProgramHandle BgfxShaderManager::resolve_program(const std::string& shader_name) const {
    const auto default_it = impl_->program_cache.find("mesh");
    if (default_it == impl_->program_cache.end()) {
        return BGFX_INVALID_HANDLE;
    }

    if (auto it = impl_->program_cache.find(shader_name); it != impl_->program_cache.end()) {
        return it->second;
    }

    if (shader_name != "mesh") {
        LOG_EVERY_N_SEC(WARNING, 2.0)
            << "Shader not found in cache: " << shader_name << ", falling back to default";
    }
    return default_it->second;
}

bgfx::ProgramHandle
BgfxShaderManager::resolve_instanced_program(const std::string& shader_name) const {
    if (auto it = impl_->instanced_program_cache.find(shader_name);
        it != impl_->instanced_program_cache.end()) {
        return it->second;
    }

    // Fall back to the default instanced program ("mesh") for unknown shader names,
    // without logging 窶・this is the expected path for batching unknown shaders.
    // (resolve_program() has a similar fallback but logs a warning; here we stay
    // silent because the renderer's per-batch fallback already warns if needed.)
    const auto default_it = impl_->instanced_program_cache.find("mesh");
    if (default_it != impl_->instanced_program_cache.end()) {
        return default_it->second;
    }
    return BGFX_INVALID_HANDLE;
}

bgfx::ProgramHandle BgfxShaderManager::resolve_skinned_program(const std::string& shader_name) const {
    if (auto it = impl_->skinned_program_cache.find(shader_name);
        it != impl_->skinned_program_cache.end()) {
        return it->second;
    }

    const auto default_it = impl_->skinned_program_cache.find("mesh");
    if (default_it != impl_->skinned_program_cache.end()) {
        return default_it->second;
    }
    return BGFX_INVALID_HANDLE;
}

std::size_t BgfxShaderManager::active_program_cache_count() const {
    return impl_->program_cache.size() + impl_->instanced_program_cache.size() +
           impl_->skinned_program_cache.size();
}

} // namespace isla::client

#endif // defined(_WIN32)
