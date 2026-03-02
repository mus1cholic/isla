#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bgfx {
struct IndexBufferHandle;
struct VertexBufferHandle;
} // namespace bgfx

namespace isla::client {

class RenderWorld;

class BgfxMeshManager {
  public:
    BgfxMeshManager();
    ~BgfxMeshManager();

    BgfxMeshManager(const BgfxMeshManager&) = delete;
    BgfxMeshManager& operator=(const BgfxMeshManager&) = delete;
    BgfxMeshManager(BgfxMeshManager&&) = delete;
    BgfxMeshManager& operator=(BgfxMeshManager&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();

    void begin_frame();
    void upload_dirty_meshes(const RenderWorld& world);

    [[nodiscard]] bool has_mesh_slot(std::size_t mesh_id) const;
    [[nodiscard]] bgfx::VertexBufferHandle vertex_buffer_for_mesh(std::size_t mesh_id) const;
    [[nodiscard]] bgfx::IndexBufferHandle index_buffer_for_mesh(std::size_t mesh_id) const;

    [[nodiscard]] std::size_t uploaded_mesh_count() const;
    [[nodiscard]] std::size_t last_frame_mesh_upload_count() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace isla::client
