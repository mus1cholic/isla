#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace bgfx {
struct ProgramHandle;
}

namespace isla::client {

class BgfxShaderManager {
  public:
    BgfxShaderManager();
    ~BgfxShaderManager();

    BgfxShaderManager(const BgfxShaderManager&) = delete;
    BgfxShaderManager& operator=(const BgfxShaderManager&) = delete;
    BgfxShaderManager(BgfxShaderManager&&) = delete;
    BgfxShaderManager& operator=(BgfxShaderManager&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();

    [[nodiscard]] bgfx::ProgramHandle resolve_program(const std::string& shader_name) const;
    [[nodiscard]] bgfx::ProgramHandle
    resolve_instanced_program(const std::string& shader_name) const;
    [[nodiscard]] std::size_t active_program_cache_count() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::client
