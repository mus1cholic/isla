#include "engine/src/render/model_renderer_test_hooks.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "isla/engine/render/render_world.hpp"

namespace isla::client {
namespace {

TEST(ModelRendererDebugOverlayTest, StoresOverlayStateWithoutInitialization) {
    ModelRenderer renderer;

    EXPECT_FALSE(internal::ModelRendererTestHooks::debug_overlay_enabled(renderer));
    EXPECT_TRUE(internal::ModelRendererTestHooks::debug_overlay_lines(renderer).empty());

    const std::vector<std::string> overlay_lines = {
        "Gateway Debug HUD",
        "Connected: yes",
        "Reply: hello",
    };
    renderer.set_debug_overlay_enabled(true);
    renderer.set_debug_overlay_lines(overlay_lines);
    renderer.render(RenderWorld{});

    EXPECT_TRUE(internal::ModelRendererTestHooks::debug_overlay_enabled(renderer));
    EXPECT_EQ(internal::ModelRendererTestHooks::debug_overlay_lines(renderer), overlay_lines);
}

} // namespace
} // namespace isla::client
