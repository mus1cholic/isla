#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace bgfx {
struct TextureHandle;
}

namespace isla::client {

class BgfxTextureManager {
  public:
    BgfxTextureManager();
    ~BgfxTextureManager();

    BgfxTextureManager(const BgfxTextureManager&) = delete;
    BgfxTextureManager& operator=(const BgfxTextureManager&) = delete;
    BgfxTextureManager(BgfxTextureManager&&) = delete;
    BgfxTextureManager& operator=(BgfxTextureManager&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();

    [[nodiscard]] bgfx::TextureHandle resolve_texture(std::string_view texture_path);
    [[nodiscard]] bool has_valid_default_texture() const;
    [[nodiscard]] std::size_t active_texture_cache_count() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::client
