#include "client_sdl_runtime.hpp"

#include <SDL3/SDL.h>

namespace isla::client {

std::uint64_t SdlRuntime::get_ticks_ns() const {
    return SDL_GetTicksNS();
}

bool SdlRuntime::init_video() const {
    return SDL_Init(SDL_INIT_VIDEO);
}

void SdlRuntime::quit() const {
    SDL_Quit();
}

bool SdlRuntime::has_primary_display() const {
    return SDL_GetPrimaryDisplay() != 0;
}

SDL_Window* SdlRuntime::create_window(const char* title, int width, int height,
                                      std::uint64_t flags) const {
    return SDL_CreateWindow(title, width, height, flags);
}

SDL_Renderer* SdlRuntime::create_renderer(SDL_Window* window) const {
    return SDL_CreateRenderer(window, nullptr);
}

void SdlRuntime::destroy_renderer(SDL_Renderer* renderer) const {
    SDL_DestroyRenderer(renderer);
}

void SdlRuntime::destroy_window(SDL_Window* window) const {
    SDL_DestroyWindow(window);
}

void SdlRuntime::maximize_window(SDL_Window* window) const {
    SDL_MaximizeWindow(window);
}

bool SdlRuntime::poll_event(SDL_Event* event) const {
    return SDL_PollEvent(event);
}

const bool* SdlRuntime::get_keyboard_state(int* key_count) const {
    return SDL_GetKeyboardState(key_count);
}

bool SdlRuntime::get_window_size_in_pixels(SDL_Window* window, int* width, int* height) const {
    return SDL_GetWindowSizeInPixels(window, width, height);
}

bool SdlRuntime::get_window_size(SDL_Window* window, int* width, int* height) const {
    return SDL_GetWindowSize(window, width, height);
}

void SdlRuntime::set_window_bordered(SDL_Window* window, bool bordered) const {
    SDL_SetWindowBordered(window, bordered);
}

bool SdlRuntime::set_window_relative_mouse_mode(SDL_Window* window, bool enabled) const {
    return SDL_SetWindowRelativeMouseMode(window, enabled);
}

bool SdlRuntime::start_text_input(SDL_Window* window) const {
    return SDL_StartTextInput(window);
}

void SdlRuntime::stop_text_input(SDL_Window* window) const {
    SDL_StopTextInput(window);
}

const ISdlRuntime& default_sdl_runtime() {
    static const SdlRuntime instance{};
    return instance;
}

} // namespace isla::client
