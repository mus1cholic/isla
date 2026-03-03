#include <gtest/gtest.h>

#if defined(_WIN32)

#include <bgfx/bgfx.h>

#include "engine/src/render/include/bgfx_mesh_manager.hpp"
#include "isla/engine/render/render_world.hpp"

namespace {

class BgfxMeshManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bgfx::Init init{};
        init.type = bgfx::RendererType::Noop;
        init.resolution.width = 16U;
        init.resolution.height = 16U;
        init.resolution.reset = 0U;
        ASSERT_TRUE(bgfx::init(init));
    }

    void TearDown() override {
        bgfx::shutdown();
    }
};

TEST_F(BgfxMeshManagerTest, SkinnedMeshUploadsOnceThenSkipsPaletteOnlyUpdates) {
    isla::client::RenderWorld world;
    isla::client::MeshData mesh;
    mesh.set_skinned_geometry(
        {
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 0.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 1.0F, .y = 0.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 1.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
        },
        { 0U, 1U, 2U });
    mesh.set_skin_palette({ isla::client::Mat4::identity() });
    world.meshes().push_back(std::move(mesh));

    isla::client::BgfxMeshManager manager;
    ASSERT_TRUE(manager.initialize());

    manager.begin_frame();
    manager.upload_dirty_meshes(world);
    EXPECT_EQ(manager.last_frame_mesh_upload_count(), 1U);
    EXPECT_EQ(manager.uploaded_mesh_count(), 1U);
    EXPECT_TRUE(manager.has_mesh_slot(0U));
    EXPECT_TRUE(manager.mesh_is_skinned(0U));

    // Palette-only update: geometry revision is unchanged, so mesh buffers should not reupload.
    world.meshes()[0].set_skin_palette(
        { isla::client::Mat4::translation(isla::client::Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F }) });
    manager.begin_frame();
    manager.upload_dirty_meshes(world);
    EXPECT_EQ(manager.last_frame_mesh_upload_count(), 0U);
    EXPECT_EQ(manager.uploaded_mesh_count(), 1U);

    manager.shutdown();
}

TEST_F(BgfxMeshManagerTest, SkinnedMeshWithInvalidIndicesIsRejected) {
    isla::client::RenderWorld world;
    isla::client::MeshData mesh;
    mesh.set_skinned_geometry(
        {
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 0.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 1.0F, .y = 0.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 1.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
        },
        { 0U, 1U, 9U });
    mesh.set_skin_palette({ isla::client::Mat4::identity() });
    world.meshes().push_back(std::move(mesh));

    isla::client::BgfxMeshManager manager;
    ASSERT_TRUE(manager.initialize());

    manager.begin_frame();
    manager.upload_dirty_meshes(world);
    EXPECT_EQ(manager.last_frame_mesh_upload_count(), 0U);
    EXPECT_EQ(manager.uploaded_mesh_count(), 0U);
    EXPECT_TRUE(manager.has_mesh_slot(0U));
    EXPECT_TRUE(manager.mesh_is_skinned(0U));
    EXPECT_FALSE(bgfx::isValid(manager.vertex_buffer_for_mesh(0U)));
    EXPECT_FALSE(bgfx::isValid(manager.index_buffer_for_mesh(0U)));

    manager.shutdown();
}

TEST_F(BgfxMeshManagerTest, SkinnedMeshWithJointBeyondGpuPaletteLimitIsRejected) {
    isla::client::RenderWorld world;
    isla::client::MeshData mesh;
    mesh.set_skinned_geometry(
        {
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 0.0F },
                .joints = { 64U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 1.0F, .y = 0.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
            isla::client::SkinnedMeshVertex{
                .position = isla::client::Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .normal = isla::client::Vec3{ .x = 0.0F, .y = 0.0F, .z = 1.0F },
                .uv = isla::client::Vec2{ .x = 0.0F, .y = 1.0F },
                .joints = { 0U, 0U, 0U, 0U },
                .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
            },
        },
        { 0U, 1U, 2U });
    mesh.set_skin_palette({ isla::client::Mat4::identity() });
    world.meshes().push_back(std::move(mesh));

    isla::client::BgfxMeshManager manager;
    ASSERT_TRUE(manager.initialize());

    manager.begin_frame();
    manager.upload_dirty_meshes(world);
    EXPECT_EQ(manager.last_frame_mesh_upload_count(), 0U);
    EXPECT_EQ(manager.uploaded_mesh_count(), 0U);
    EXPECT_TRUE(manager.has_mesh_slot(0U));
    EXPECT_TRUE(manager.mesh_is_skinned(0U));
    EXPECT_FALSE(bgfx::isValid(manager.vertex_buffer_for_mesh(0U)));
    EXPECT_FALSE(bgfx::isValid(manager.index_buffer_for_mesh(0U)));

    manager.shutdown();
}

} // namespace

#else

TEST(BgfxMeshManagerTest, SkippedOnNonWindows) {
    SUCCEED();
}

#endif
