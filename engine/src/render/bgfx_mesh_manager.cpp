#include "engine/src/render/include/bgfx_mesh_manager.hpp"

#if defined(_WIN32)

#include "engine/src/render/include/bgfx_limits.hpp"
#include "isla/engine/render/render_world.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "absl/log/log.h"
#include <bgfx/bgfx.h>

#include "isla/engine/math/render_math.hpp"

namespace isla::client {

using namespace render_math;

namespace {

constexpr std::size_t kTriangleVertexCount = 3U;
constexpr std::size_t kNormalPackedElementCount = 4U;

struct MeshVertex {
    float x;
    float y;
    float z;
    std::array<std::uint8_t, kNormalPackedElementCount> normal;
    float u;
    float v;

    static void init_layout(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true, false)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
    }
};

struct GpuSkinnedMeshVertex {
    float x;
    float y;
    float z;
    std::array<std::uint8_t, kNormalPackedElementCount> normal;
    float u;
    float v;
    std::array<float, 4U> joints;
    std::array<float, 4U> weights;

    static void init_layout(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true, false)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Indices, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Weight, 4, bgfx::AttribType::Float)
            .end();
    }
};

std::uint8_t pack_normal_component(float component) {
    const float clamped = std::clamp(component, -1.0F, 1.0F);
    const float mapped = (clamped * 0.5F) + 0.5F;
    const float scaled = mapped * 255.0F;
    return static_cast<std::uint8_t>(std::clamp(scaled, 0.0F, 255.0F));
}

std::array<std::uint8_t, kNormalPackedElementCount> pack_normal(const Vec3& normal) {
    return {
        pack_normal_component(normal.x),
        pack_normal_component(normal.y),
        pack_normal_component(normal.z),
        std::numeric_limits<std::uint8_t>::max(),
    };
}

void destroy_vertex_buffer_if_valid(bgfx::VertexBufferHandle handle) {
    if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
    }
}

void destroy_index_buffer_if_valid(bgfx::IndexBufferHandle handle) {
    if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
    }
}

void destroy_vertex_buffers(std::vector<bgfx::VertexBufferHandle>& buffers) {
    for (bgfx::VertexBufferHandle handle : buffers) {
        destroy_vertex_buffer_if_valid(handle);
    }
    buffers.clear();
}

void destroy_index_buffers(std::vector<bgfx::IndexBufferHandle>& buffers) {
    for (bgfx::IndexBufferHandle handle : buffers) {
        destroy_index_buffer_if_valid(handle);
    }
    buffers.clear();
}

} // namespace

class BgfxMeshManager::Impl {
  public:
    std::vector<bgfx::VertexBufferHandle> mesh_vertex_buffers;
    std::vector<bgfx::IndexBufferHandle> mesh_index_buffers;
    std::vector<std::uint64_t> mesh_geometry_revisions;
    std::vector<bool> mesh_is_skinned;
    bgfx::VertexLayout mesh_vertex_layout;
    bgfx::VertexLayout skinned_mesh_vertex_layout;
    std::size_t uploaded_mesh_count = 0U;
    std::size_t last_frame_mesh_upload_count = 0U;
};

BgfxMeshManager::BgfxMeshManager() : impl_(std::make_unique<Impl>()) {}

BgfxMeshManager::~BgfxMeshManager() = default;

bool BgfxMeshManager::initialize() {
    shutdown();
    MeshVertex::init_layout(impl_->mesh_vertex_layout);
    GpuSkinnedMeshVertex::init_layout(impl_->skinned_mesh_vertex_layout);
    return true;
}

void BgfxMeshManager::shutdown() {
    destroy_vertex_buffers(impl_->mesh_vertex_buffers);
    destroy_index_buffers(impl_->mesh_index_buffers);
    impl_->mesh_geometry_revisions.clear();
    impl_->mesh_is_skinned.clear();
    impl_->uploaded_mesh_count = 0U;
    impl_->last_frame_mesh_upload_count = 0U;
}

void BgfxMeshManager::begin_frame() {
    impl_->last_frame_mesh_upload_count = 0U;
}

void BgfxMeshManager::upload_dirty_meshes(const RenderWorld& world) {
    const std::size_t mesh_count = world.meshes().size();
    if (impl_->mesh_vertex_buffers.size() != mesh_count ||
        impl_->mesh_index_buffers.size() != mesh_count ||
        impl_->mesh_geometry_revisions.size() != mesh_count) {
        VLOG(1) << "BgfxRenderer: mesh buffer topology changed, resizing tracked buffers to "
                << mesh_count;
        destroy_vertex_buffers(impl_->mesh_vertex_buffers);
        destroy_index_buffers(impl_->mesh_index_buffers);
        impl_->mesh_vertex_buffers.assign(mesh_count, BGFX_INVALID_HANDLE);
        impl_->mesh_index_buffers.assign(mesh_count, BGFX_INVALID_HANDLE);
        impl_->mesh_geometry_revisions.assign(mesh_count, 0U);
        impl_->mesh_is_skinned.assign(mesh_count, false);
    }

    for (std::size_t mesh_index = 0; mesh_index < mesh_count; ++mesh_index) {
        const MeshData& mesh = world.meshes().at(mesh_index);
        const std::uint64_t geometry_revision = mesh.geometry_revision();
        const std::uint64_t previous_geometry_revision =
            impl_->mesh_geometry_revisions.at(mesh_index);
        if (geometry_revision == 0U && !mesh.triangles().empty()) {
            LOG_EVERY_N_SEC(WARNING, 2.0)
                << "BgfxRenderer: mesh " << mesh_index
                << " has non-empty geometry but revision=0; expected positive revision and "
                   "MeshData mutation via edit APIs "
                   "(edit_triangles/set_triangles/append_triangle/clear_triangles)";
        }
        if (previous_geometry_revision == geometry_revision) {
            VLOG(2) << "BgfxRenderer: mesh " << mesh_index
                    << " revision unchanged, reusing existing GPU buffers";
            continue;
        }

        if (mesh.has_skinned_geometry()) {
            const std::size_t vertex_count = mesh.skinned_vertices().size();
            const std::size_t index_count = mesh.skinned_indices().size();
            if (vertex_count == 0U || index_count == 0U) {
                destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
                destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
                impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
                impl_->mesh_is_skinned.at(mesh_index) = true;
                continue;
            }
            if (vertex_count > (internal::kMaxBgfxCopyBytes / sizeof(GpuSkinnedMeshVertex))) {
                destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
                destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
                impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
                impl_->mesh_is_skinned.at(mesh_index) = true;
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "BgfxRenderer: skinned mesh vertex buffer byte size exceeds "
                       "bgfx::copy limit, skipping mesh";
                continue;
            }
            if (index_count > (internal::kMaxBgfxCopyBytes / sizeof(std::uint32_t))) {
                destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
                destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
                impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
                impl_->mesh_is_skinned.at(mesh_index) = true;
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "BgfxRenderer: skinned mesh index buffer byte size exceeds "
                       "bgfx::copy limit, skipping mesh";
                continue;
            }
            bool has_invalid_index = false;
            for (const std::uint32_t index : mesh.skinned_indices()) {
                if (static_cast<std::size_t>(index) >= vertex_count) {
                    has_invalid_index = true;
                    break;
                }
            }
            if (has_invalid_index) {
                destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
                destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
                impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
                impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
                impl_->mesh_is_skinned.at(mesh_index) = true;
                LOG(WARNING) << "BgfxRenderer: skinned mesh has out-of-range index, skipping mesh";
                continue;
            }

            std::vector<GpuSkinnedMeshVertex> vertices;
            vertices.reserve(vertex_count);
            for (const isla::client::SkinnedMeshVertex& vertex : mesh.skinned_vertices()) {
                vertices.push_back(GpuSkinnedMeshVertex{
                    .x = vertex.position.x,
                    .y = vertex.position.y,
                    .z = vertex.position.z,
                    .normal = pack_normal(normalize(vertex.normal)),
                    .u = vertex.uv.x,
                    .v = vertex.uv.y,
                    .joints = {
                        static_cast<float>(vertex.joints[0]),
                        static_cast<float>(vertex.joints[1]),
                        static_cast<float>(vertex.joints[2]),
                        static_cast<float>(vertex.joints[3]),
                    },
                    .weights = vertex.weights,
                });
            }
            std::vector<std::uint32_t> indices = mesh.skinned_indices();

            const std::size_t vertex_bytes = vertices.size() * sizeof(GpuSkinnedMeshVertex);
            const std::size_t index_bytes = indices.size() * sizeof(std::uint32_t);
            const bgfx::Memory* vertex_memory =
                bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertex_bytes));
            const bgfx::Memory* index_memory =
                bgfx::copy(indices.data(), static_cast<std::uint32_t>(index_bytes));
            bgfx::VertexBufferHandle new_vertex_buffer =
                bgfx::createVertexBuffer(vertex_memory, impl_->skinned_mesh_vertex_layout);
            bgfx::IndexBufferHandle new_index_buffer =
                bgfx::createIndexBuffer(index_memory, BGFX_BUFFER_INDEX32);
            if (!bgfx::isValid(new_vertex_buffer) || !bgfx::isValid(new_index_buffer)) {
                if (bgfx::isValid(new_vertex_buffer)) {
                    bgfx::destroy(new_vertex_buffer);
                }
                if (bgfx::isValid(new_index_buffer)) {
                    bgfx::destroy(new_index_buffer);
                }
                LOG_EVERY_N_SEC(WARNING, 2.0)
                    << "BgfxRenderer: failed to create skinned mesh buffers for mesh "
                    << mesh_index << " at revision " << geometry_revision
                    << "; keeping previous GPU buffers and retrying on future frames";
                continue;
            }
            destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
            destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
            impl_->mesh_vertex_buffers.at(mesh_index) = new_vertex_buffer;
            impl_->mesh_index_buffers.at(mesh_index) = new_index_buffer;
            impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
            impl_->mesh_is_skinned.at(mesh_index) = true;
            ++impl_->uploaded_mesh_count;
            ++impl_->last_frame_mesh_upload_count;
            VLOG(1) << "BgfxRenderer: uploaded skinned mesh " << mesh_index
                    << " vertices=" << vertex_count << ", indices=" << index_count
                    << ", revision " << previous_geometry_revision << " -> " << geometry_revision
                    << ", total_uploads=" << impl_->uploaded_mesh_count;
            continue;
        }

        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;

        const std::size_t triangle_count = mesh.triangles().size();
        if (triangle_count == 0U) {
            destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
            destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
            impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
            VLOG(1) << "BgfxRenderer: mesh " << mesh_index
                    << " has zero triangles, cleared GPU buffers and updated revision "
                    << previous_geometry_revision << " -> " << geometry_revision;
            continue;
        }
        if (triangle_count > (std::numeric_limits<std::uint32_t>::max() / kTriangleVertexCount)) {
            destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
            destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
            impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
            LOG(WARNING) << "BgfxRenderer: mesh exceeds 32-bit index limit, skipping mesh";
            continue;
        }

        // TODO(isla): Extract mesh upload limit math into a small pure helper and add
        // boundary-focused unit tests for the rare overflow paths (triangle_count/index_count/
        // vertex_count and byte-size limits for bgfx::copy) so max and max+1 behavior can be
        // validated without allocating pathological mesh buffers in integration tests.
        const std::size_t vertex_count = triangle_count * kTriangleVertexCount;
        const std::size_t index_count = triangle_count * kTriangleVertexCount;
        if (vertex_count > (internal::kMaxBgfxCopyBytes / sizeof(MeshVertex))) {
            destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
            destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
            impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
            LOG(WARNING) << "BgfxRenderer: mesh vertex buffer byte size exceeds bgfx::copy limit, "
                            "skipping mesh";
            continue;
        }
        if (index_count > (internal::kMaxBgfxCopyBytes / sizeof(std::uint32_t))) {
            destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
            destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
            impl_->mesh_vertex_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_index_buffers.at(mesh_index) = BGFX_INVALID_HANDLE;
            impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
            LOG(WARNING) << "BgfxRenderer: mesh index buffer byte size exceeds bgfx::copy limit, "
                            "skipping mesh";
            continue;
        }

        vertices.reserve(vertex_count);
        indices.reserve(index_count);

        std::uint32_t running_index = 0U;
        for (const Triangle& triangle : mesh.triangles()) {
            const Vec3 edge_ab = triangle.b - triangle.a;
            const Vec3 edge_ac = triangle.c - triangle.a;
            const Vec3 normal = normalize(cross(edge_ab, edge_ac));
            const std::array<std::uint8_t, kNormalPackedElementCount> packed_normal =
                pack_normal(normal);

            const MeshVertex a = MeshVertex{
                .x = triangle.a.x,
                .y = triangle.a.y,
                .z = triangle.a.z,
                .normal = packed_normal,
                .u = triangle.uv_a.x,
                .v = triangle.uv_a.y,
            };
            const MeshVertex b = MeshVertex{
                .x = triangle.b.x,
                .y = triangle.b.y,
                .z = triangle.b.z,
                .normal = packed_normal,
                .u = triangle.uv_b.x,
                .v = triangle.uv_b.y,
            };
            const MeshVertex c = MeshVertex{
                .x = triangle.c.x,
                .y = triangle.c.y,
                .z = triangle.c.z,
                .normal = packed_normal,
                .u = triangle.uv_c.x,
                .v = triangle.uv_c.y,
            };
            vertices.push_back(a);
            vertices.push_back(b);
            vertices.push_back(c);

            indices.push_back(running_index++);
            indices.push_back(running_index++);
            indices.push_back(running_index++);
        }

        const std::size_t vertex_bytes = vertices.size() * sizeof(MeshVertex);
        const std::size_t index_bytes = indices.size() * sizeof(std::uint32_t);
        const bgfx::Memory* vertex_memory =
            bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertex_bytes));
        const bgfx::Memory* index_memory =
            bgfx::copy(indices.data(), static_cast<std::uint32_t>(index_bytes));
        bgfx::VertexBufferHandle new_vertex_buffer =
            bgfx::createVertexBuffer(vertex_memory, impl_->mesh_vertex_layout);
        bgfx::IndexBufferHandle new_index_buffer =
            bgfx::createIndexBuffer(index_memory, BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(new_vertex_buffer) || !bgfx::isValid(new_index_buffer)) {
            if (bgfx::isValid(new_vertex_buffer)) {
                bgfx::destroy(new_vertex_buffer);
            }
            if (bgfx::isValid(new_index_buffer)) {
                bgfx::destroy(new_index_buffer);
            }
            LOG_EVERY_N_SEC(WARNING, 2.0)
                << "BgfxRenderer: failed to create mesh buffers for mesh " << mesh_index
                << " at revision " << geometry_revision
                << "; keeping previous GPU buffers and retrying on future frames";
            continue;
        }
        destroy_vertex_buffer_if_valid(impl_->mesh_vertex_buffers.at(mesh_index));
        destroy_index_buffer_if_valid(impl_->mesh_index_buffers.at(mesh_index));
        impl_->mesh_vertex_buffers.at(mesh_index) = new_vertex_buffer;
        impl_->mesh_index_buffers.at(mesh_index) = new_index_buffer;
        impl_->mesh_geometry_revisions.at(mesh_index) = geometry_revision;
        impl_->mesh_is_skinned.at(mesh_index) = false;
        ++impl_->uploaded_mesh_count;
        ++impl_->last_frame_mesh_upload_count;
        VLOG(1) << "BgfxRenderer: uploaded mesh " << mesh_index << " triangles=" << triangle_count
                << ", revision " << previous_geometry_revision << " -> " << geometry_revision
                << ", total_uploads=" << impl_->uploaded_mesh_count;
    }
}

bool BgfxMeshManager::has_mesh_slot(std::size_t mesh_id) const {
    return mesh_id < impl_->mesh_vertex_buffers.size() &&
           mesh_id < impl_->mesh_index_buffers.size();
}

bool BgfxMeshManager::mesh_is_skinned(std::size_t mesh_id) const {
    return mesh_id < impl_->mesh_is_skinned.size() && impl_->mesh_is_skinned.at(mesh_id);
}

bgfx::VertexBufferHandle BgfxMeshManager::vertex_buffer_for_mesh(std::size_t mesh_id) const {
    if (!has_mesh_slot(mesh_id)) {
        return BGFX_INVALID_HANDLE;
    }
    return impl_->mesh_vertex_buffers.at(mesh_id);
}

bgfx::IndexBufferHandle BgfxMeshManager::index_buffer_for_mesh(std::size_t mesh_id) const {
    if (!has_mesh_slot(mesh_id)) {
        return BGFX_INVALID_HANDLE;
    }
    return impl_->mesh_index_buffers.at(mesh_id);
}

std::size_t BgfxMeshManager::uploaded_mesh_count() const {
    return impl_->uploaded_mesh_count;
}

std::size_t BgfxMeshManager::last_frame_mesh_upload_count() const {
    return impl_->last_frame_mesh_upload_count;
}

} // namespace isla::client

#endif // defined(_WIN32)
