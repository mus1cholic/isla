#pragma once

struct SDL_Window;

namespace isla::client {

[[nodiscard]] bool configure_win32_layered_overlay(SDL_Window* window);
[[nodiscard]] bool refresh_win32_layered_overlay_surface(SDL_Window* window);

} // namespace isla::client
