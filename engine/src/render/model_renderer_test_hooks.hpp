#pragma once

#include "isla/engine/render/model_renderer.hpp"

#include <string>
#include <vector>

namespace isla::client::internal {

class ModelRendererTestHooks {
  public:
    static bool debug_overlay_enabled(const ModelRenderer& renderer);
    static std::vector<std::string> debug_overlay_lines(const ModelRenderer& renderer);
};

} // namespace isla::client::internal
