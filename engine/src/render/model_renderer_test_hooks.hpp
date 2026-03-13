#pragma once

#include "isla/engine/render/model_renderer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace isla::client::internal {

class ModelRendererTestHooks {
  public:
    static bool debug_overlay_enabled(const ModelRenderer& renderer);
    static std::vector<std::string> debug_overlay_lines(const ModelRenderer& renderer);
    static ChatPanelState chat_panel_state(const ModelRenderer& renderer);
    static void queue_chat_submit(ModelRenderer& renderer, std::string text);
};

} // namespace isla::client::internal
