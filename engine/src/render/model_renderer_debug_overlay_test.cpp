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

TEST(ModelRendererDebugOverlayTest, StoresChatPanelStateAndPendingSubmitWithoutInitialization) {
    ModelRenderer renderer;

    ChatPanelState chat_panel_state{
        .enabled = true,
        .connected = true,
        .turn_in_flight = false,
        .status_line = "Connected",
        .transcript = {
            ChatPanelEntry{ .role = ChatPanelEntryRole::User, .text = "hello" },
            ChatPanelEntry{ .role = ChatPanelEntryRole::Assistant, .text = "hi there" },
        },
    };

    renderer.set_chat_panel_state(chat_panel_state);
    EXPECT_EQ(internal::ModelRendererTestHooks::chat_panel_state(renderer).status_line,
              "Connected");
    ASSERT_EQ(internal::ModelRendererTestHooks::chat_panel_state(renderer).transcript.size(), 2U);

    internal::ModelRendererTestHooks::queue_chat_submit(renderer, "hello from hook");
    EXPECT_EQ(renderer.take_chat_submit_request(), std::optional<std::string>("hello from hook"));
    EXPECT_FALSE(renderer.take_chat_submit_request().has_value());
}

} // namespace
} // namespace isla::client
