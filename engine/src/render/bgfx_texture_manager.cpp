#include "engine/src/render/include/bgfx_texture_manager.hpp"

#if defined(_WIN32)

#include "engine/src/render/include/bgfx_limits.hpp"
#include "engine/src/render/include/texture_loader_limits.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/log/log.h"
#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bimg/bimg.h>
#include <bimg/decode.h>
#include <bx/allocator.h>

namespace isla::client {

class BgfxTextureManager::Impl {
  public:
    std::unordered_map<std::string, bgfx::TextureHandle> texture_cache;
    bgfx::TextureHandle default_texture = BGFX_INVALID_HANDLE;
};

namespace {

std::string resolve_texture_path(std::string_view texture_path) {
    if (texture_path.empty()) {
        return {};
    }

    const std::string texture_path_string(texture_path);
    if (std::filesystem::path(texture_path_string).is_absolute()) {
        return texture_path_string;
    }

    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr || base_path[0] == '\0') {
        return texture_path_string;
    }

    const std::filesystem::path resolved =
        std::filesystem::path(base_path) / std::filesystem::path(texture_path_string);
    return resolved.lexically_normal().string();
}

bgfx::TextureHandle load_texture_from_file(std::string_view texture_path) {
    const std::string texture_path_string(texture_path);
    const std::string resolved_texture_path = resolve_texture_path(texture_path);
    std::ifstream stream(resolved_texture_path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        LOG(ERROR) << "BgfxRenderer: failed to open texture '" << texture_path_string
                   << "' (resolved to '" << resolved_texture_path << "')";
        return BGFX_INVALID_HANDLE;
    }

    const std::ifstream::pos_type length = stream.tellg();
    if (length <= 0) {
        LOG(ERROR) << "BgfxRenderer: texture '" << texture_path_string << "' is empty";
        return BGFX_INVALID_HANDLE;
    }

    const auto encoded_size_bytes = static_cast<std::uint64_t>(length);
    if (!internal::is_encoded_texture_size_supported(encoded_size_bytes)) {
        LOG(ERROR) << "BgfxRenderer: texture '" << texture_path_string << "' encoded size "
                   << encoded_size_bytes << " exceeds supported max of "
                   << internal::kMaxEncodedTextureBytes;
        return BGFX_INVALID_HANDLE;
    }
    stream.seekg(0, std::ios::beg);

    const std::size_t byte_count = static_cast<std::size_t>(length);
    std::vector<std::uint8_t> encoded_data(byte_count);
    if (!stream.read(reinterpret_cast<char*>(encoded_data.data()),
                     static_cast<std::streamsize>(length))) {
        LOG(ERROR) << "BgfxRenderer: failed to read texture '" << texture_path_string << "'";
        return BGFX_INVALID_HANDLE;
    }

    // Fast path: let bgfx handle pre-encoded GPU texture containers (DDS/KTX/PVR).
    {
        const bgfx::Memory* encoded_memory =
            bgfx::copy(encoded_data.data(), static_cast<std::uint32_t>(encoded_data.size()));
        bgfx::TextureHandle texture = bgfx::createTexture(encoded_memory);
        if (bgfx::isValid(texture)) {
            return texture;
        }
    }

    bx::DefaultAllocator allocator;
    bimg::ImageContainer* decoded = bimg::imageParse(
        &allocator, encoded_data.data(), static_cast<std::uint32_t>(encoded_data.size()),
        bimg::TextureFormat::RGBA8);
    if (decoded == nullptr) {
        LOG(ERROR) << "BgfxRenderer: failed to decode texture '" << texture_path_string
                   << "' (resolved to '" << resolved_texture_path << "')";
        return BGFX_INVALID_HANDLE;
    }
    if (decoded->m_data == nullptr || decoded->m_width == 0U || decoded->m_height == 0U) {
        LOG(ERROR) << "BgfxRenderer: failed to decode texture '" << texture_path_string
                   << "' (resolved to '" << resolved_texture_path << "')";
        bimg::imageFree(decoded);
        return BGFX_INVALID_HANDLE;
    }

    const std::size_t width = decoded->m_width;
    const std::size_t height = decoded->m_height;
    if (width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
        LOG(ERROR) << "BgfxRenderer: texture '" << texture_path_string << "' dimensions " << width
                   << "x" << height << " exceed bgfx texture2D limits";
        bimg::imageFree(decoded);
        return BGFX_INVALID_HANDLE;
    }
    if (decoded->m_size == 0U || decoded->m_size > internal::kMaxBgfxCopyBytes) {
        LOG(ERROR) << "BgfxRenderer: texture '" << texture_path_string << "' uses "
                   << decoded->m_size << " bytes, exceeding bgfx::copy max of "
                   << internal::kMaxBgfxCopyBytes;
        bimg::imageFree(decoded);
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* texture_memory =
        bgfx::copy(decoded->m_data, static_cast<std::uint32_t>(decoded->m_size));
    bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
                              false, 1, bgfx::TextureFormat::RGBA8, 0, texture_memory);
    bimg::imageFree(decoded);
    return texture;
}

} // namespace

BgfxTextureManager::BgfxTextureManager() : impl_(std::make_unique<Impl>()) {}

BgfxTextureManager::~BgfxTextureManager() = default;

bool BgfxTextureManager::initialize() {
    shutdown();

    const std::uint32_t white_pixel = 0xFFFFFFFF;
    impl_->default_texture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
                                                   BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE,
                                                   bgfx::copy(&white_pixel, sizeof(white_pixel)));
    if (!bgfx::isValid(impl_->default_texture)) {
        LOG(ERROR) << "BgfxRenderer: initialize failed: could not create default texture";
        return false;
    }
    return true;
}

void BgfxTextureManager::shutdown() {
    for (auto& [path, texture] : impl_->texture_cache) {
        if (bgfx::isValid(texture) && texture.idx != impl_->default_texture.idx) {
            bgfx::destroy(texture);
        }
    }
    impl_->texture_cache.clear();
    if (bgfx::isValid(impl_->default_texture)) {
        bgfx::destroy(impl_->default_texture);
        impl_->default_texture = BGFX_INVALID_HANDLE;
    }
}

bgfx::TextureHandle BgfxTextureManager::resolve_texture(std::string_view texture_path) {
    if (!bgfx::isValid(impl_->default_texture)) {
        return BGFX_INVALID_HANDLE;
    }
    if (texture_path.empty()) {
        return impl_->default_texture;
    }

    const std::string key(texture_path);
    if (auto it = impl_->texture_cache.find(key); it != impl_->texture_cache.end()) {
        return it->second;
    }

    bgfx::TextureHandle loaded_texture = load_texture_from_file(texture_path);
    if (!bgfx::isValid(loaded_texture)) {
        LOG(ERROR) << "BgfxRenderer: failed to create texture for path '" << key
                   << "', caching default texture fallback";
        loaded_texture = impl_->default_texture;
    } else {
        LOG(INFO) << "BgfxRenderer: loaded texture '" << key << "' into cache";
    }

    impl_->texture_cache.emplace(key, loaded_texture);
    return loaded_texture;
}

bool BgfxTextureManager::has_valid_default_texture() const {
    return bgfx::isValid(impl_->default_texture);
}

std::size_t BgfxTextureManager::active_texture_cache_count() const {
    return impl_->texture_cache.size();
}

} // namespace isla::client

#endif // defined(_WIN32)
