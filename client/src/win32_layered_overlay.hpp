#pragma once

struct SDL_Window;

namespace isla::client {

[[nodiscard]] bool configure_win32_alpha_composited_overlay(SDL_Window* window);
[[nodiscard]] bool refresh_win32_alpha_composited_overlay(SDL_Window* window);
[[nodiscard]] bool set_win32_overlay_input_passthrough(SDL_Window* window, bool enabled);

} // namespace isla::client
